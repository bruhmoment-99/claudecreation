/* secureboot.c - Phase 10: a Secure Boot lab, no signing tools needed.
 *
 * Secure Boot = the firmware refuses to run any program whose hash or
 * signature isn't listed in the "db" variable. The keys behind it:
 *   PK  (Platform Key)      - owner of the platform; signs changes to KEK
 *   KEK (Key Exchange Key)  - signs changes to db
 *   db  (allowed list)      - certificates or SHA-256 hashes of allowed programs
 * All three are UEFI variables in the flash chip, written only by the
 * variable driver in SMM (Phase 6). While no PK is enrolled the platform is
 * in SETUP MODE and enforces nothing; enrolling a PK switches to USER MODE.
 *
 * First boot (setup mode): enroll db = SHA-256 hashes of this app and of
 * trusted.efi, then KEK and PK, then test. Second boot (user mode): the
 * firmware checks THIS app against db before starting it; test again.
 * Reset by deleting ovmf_vars.fd (the emulated flash chip).
 */
#include "efi.h"
#include "lib.h"
#include "sha256.h"
#include "../../phase4-smm/LabSmmPkg/Include/Guid/LabSmmComm.h"

static EFI_BOOT_SERVICES *BS;
static EFI_HANDLE gSelf;

static EFI_GUID GLOBAL_VAR   = {0x8BE4DF61,0x93CA,0x11d2,{0xAA,0x0D,0x00,0xE0,0x98,0x03,0x2B,0x8C}};
static EFI_GUID IMAGE_SEC_DB = {0xd719b2cb,0x3d3a,0x4596,{0xa3,0xbc,0xda,0xd0,0x0e,0x67,0x65,0x6f}};
static EFI_GUID CERT_SHA256  = {0xc1c41626,0x504c,0x4092,{0xac,0xa9,0x41,0xf9,0x36,0x93,0x43,0x28}};
static EFI_GUID CERT_X509    = {0xa5c059a1,0x94e4,0x4aa7,{0x87,0xb5,0xab,0x15,0x5c,0x2b,0xf0,0x72}};
static EFI_GUID CERT_PKCS7   = {0x4aafd29d,0x68df,0x49ee,{0x8a,0xa9,0x34,0x7d,0x37,0x56,0x65,0xa7}};
static EFI_GUID LAB_OWNER    = LAB_SMM_COMM_GUID;   /* "who added this entry" */

#define AUTH_VAR_ATTRS (EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS | \
                        EFI_VARIABLE_RUNTIME_ACCESS | EFI_VARIABLE_TIME_BASED_AUTHENTICATED_WRITE_ACCESS)

static __attribute__((noreturn)) void halt(void) { for (;;) __asm__ volatile("hlt"); }

static const CHAR16 *status_name(EFI_STATUS s) {
    switch (s) {
        case EFI_SUCCESS:            return u"EFI_SUCCESS";
        case EFI_ACCESS_DENIED:      return u"EFI_ACCESS_DENIED";
        case EFI_SECURITY_VIOLATION: return u"EFI_SECURITY_VIOLATION";
        case EFI_WRITE_PROTECTED:    return u"EFI_WRITE_PROTECTED";
        case EFI_INVALID_PARAMETER:  return u"EFI_INVALID_PARAMETER";
        case EFI_NOT_FOUND:          return u"EFI_NOT_FOUND";
        default:                     return u"(other error)";
    }
}

/* ---------- files ---------- */
static EFI_HANDLE gDevice;
static EFI_STATUS read_file(const CHAR16 *path, uint8_t **out, UINTN *out_size) {
    EFI_GUID li_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID, sfs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    EFI_LOADED_IMAGE_PROTOCOL *li; EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs; EFI_FILE_PROTOCOL *root, *f; EFI_STATUS s;
    if (EFI_ERROR(s = BS->HandleProtocol(gSelf, &li_guid, (void **)&li))) return s;
    gDevice = li->DeviceHandle;
    if (EFI_ERROR(s = BS->HandleProtocol(li->DeviceHandle, &sfs_guid, (void **)&fs))) return s;
    if (EFI_ERROR(s = fs->OpenVolume(fs, &root))) return s;
    s = root->Open(root, &f, (CHAR16 *)path, EFI_FILE_MODE_READ, 0);
    root->Close(root);
    if (EFI_ERROR(s)) return s;
    uint64_t size = 0;
    f->SetPosition(f, ~0ULL); f->GetPosition(f, &size); f->SetPosition(f, 0);
    void *buf;
    if (EFI_ERROR(s = BS->AllocatePool(EfiLoaderData, size, &buf))) { f->Close(f); return s; }
    UINTN n = size;
    s = f->Read(f, &n, buf);
    f->Close(f);
    *out = buf; *out_size = n;
    return s;
}

/* Device path "<the disk we booted from>/<path>". LoadImage uses it to decide
 * the policy (images from a fixed disk must pass verification). */
static void *file_device_path(const CHAR16 *path) {
    EFI_GUID dp_guid = EFI_DEVICE_PATH_PROTOCOL_GUID;
    EFI_DEVICE_PATH_PROTOCOL *dev;
    if (EFI_ERROR(BS->HandleProtocol(gDevice, &dp_guid, (void **)&dev))) return NULL;
    UINTN dev_len = 0;
    for (EFI_DEVICE_PATH_PROTOCOL *n = dev; !(n->Type == 0x7F && n->SubType == 0xFF);
         n = (EFI_DEVICE_PATH_PROTOCOL *)((uint8_t *)n + (n->Length[0] | n->Length[1] << 8)))
        dev_len += n->Length[0] | n->Length[1] << 8;
    UINTN name_bytes = 0; while (path[name_bytes / 2]) name_bytes += 2; name_bytes += 2;
    UINTN file_len = 4 + name_bytes;
    uint8_t *dp;
    if (EFI_ERROR(BS->AllocatePool(EfiLoaderData, dev_len + file_len + 4, (void **)&dp))) return NULL;
    memcpy(dp, dev, dev_len);
    uint8_t *fn = dp + dev_len;
    fn[0] = 4; fn[1] = 4; fn[2] = (uint8_t)file_len; fn[3] = (uint8_t)(file_len >> 8);   /* MEDIA / FILEPATH */
    memcpy(fn + 4, path, name_bytes);
    uint8_t *end = fn + file_len;
    end[0] = 0x7F; end[1] = 0xFF; end[2] = 4; end[3] = 0;                               /* END */
    return dp;
}

/* ---------- the Authenticode hash: what db entries of type SHA-256 hold ----------
 * SHA-256 over the PE file, skipping the fields a signature would change:
 * the header checksum, the certificate-table directory entry and the
 * certificates themselves. Sections are hashed in file order. */
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)rd16(p) | (uint32_t)rd16(p + 2) << 16; }

static int authenticode_sha256(const uint8_t *img, UINTN size, uint8_t out[32]) {
    if (size < 0x40 || img[0] != 'M' || img[1] != 'Z') return 0;
    uint32_t pe = rd32(img + 0x3C);
    if (pe + 24 > size || rd32(img + pe) != 0x00004550) return 0;          /* "PE\0\0" */
    const uint8_t *coff = img + pe + 4;
    uint16_t nsec = rd16(coff + 2), optsz = rd16(coff + 16);
    const uint8_t *opt = coff + 20;
    if (rd16(opt) != 0x20B) return 0;                                      /* PE32+ */
    UINTN checksum = (UINTN)(opt + 64 - img);
    uint32_t ndirs = rd32(opt + 108);
    UINTN certdir = (UINTN)(opt + 112 + 4 * 8 - img);
    uint32_t headers = rd32(opt + 60);
    uint32_t certsize = ndirs > 4 ? rd32(img + certdir + 4) : 0;

    SHA256 c; sha256_init(&c);
    sha256_update(&c, img, checksum);
    if (ndirs > 4) {
        sha256_update(&c, img + checksum + 4, certdir - (checksum + 4));
        sha256_update(&c, img + certdir + 8, headers - (certdir + 8));
    } else {
        sha256_update(&c, img + checksum + 4, headers - (checksum + 4));
    }
    const uint8_t *sec = opt + optsz;
    int order[96]; int n = nsec < 96 ? nsec : 96;
    for (int i = 0; i < n; i++) order[i] = i;
    for (int i = 1; i < n; i++)                                            /* sort by file offset */
        for (int j = i; j > 0 && rd32(sec + 40 * order[j] + 20) < rd32(sec + 40 * order[j - 1] + 20); j--) {
            int t = order[j]; order[j] = order[j - 1]; order[j - 1] = t;
        }
    UINTN sum = headers;
    for (int i = 0; i < n; i++) {
        uint32_t raw = rd32(sec + 40 * order[i] + 16), ptr = rd32(sec + 40 * order[i] + 20);
        if (!raw) continue;
        if ((UINTN)ptr + raw > size) return 0;
        sha256_update(&c, img + ptr, raw);
        sum += raw;
    }
    if (size > sum + certsize) sha256_update(&c, img + sum, size - sum - certsize);
    sha256_final(&c, out);
    return 1;
}

static void print_hash8(const uint8_t *h) {
    CHAR16 b[18]; static const CHAR16 hx[] = u"0123456789abcdef";
    for (int i = 0; i < 8; i++) { b[2*i] = hx[h[i] >> 4]; b[2*i+1] = hx[h[i] & 15]; }
    b[16] = u'.'; b[17] = 0; print(b); print(u"..");
}

/* ---------- authenticated variable writes ----------
 * Data = EFI_VARIABLE_AUTHENTICATION_2 header + EFI_SIGNATURE_LIST payload.
 * In setup mode the header needs no signature (CertData empty); in user mode
 * the variable driver in SMM demands a valid PKCS#7 signature in it. */
#define AUTH2_SIZE 40   /* EFI_TIME(16) + WIN_CERTIFICATE(8) + CertType GUID(16) */
static uint8_t *make_auth_var(const uint8_t *payload, UINTN payload_len, UINTN *total) {
    uint8_t *d;
    *total = AUTH2_SIZE + payload_len;
    if (EFI_ERROR(BS->AllocatePool(EfiLoaderData, *total, (void **)&d))) return NULL;
    memset(d, 0, AUTH2_SIZE);
    EFI_TIME t;
    if (!EFI_ERROR(gST->RuntimeServices->GetTime(&t, NULL))) {
        t.Nanosecond = 0; t.TimeZone = 0; t.Daylight = 0; t.Pad1 = 0; t.Pad2 = 0;   /* required to be zero */
        memcpy(d, &t, 16);
    }
    uint32_t dwLength = 24;                         /* header + CertType, no CertData */
    memcpy(d + 16, &dwLength, 4);
    d[20] = 0x00; d[21] = 0x02;                     /* wRevision 0x0200 */
    d[22] = 0xF1; d[23] = 0x0E;                     /* WIN_CERT_TYPE_EFI_GUID */
    memcpy(d + 24, &CERT_PKCS7, 16);
    if (payload_len) memcpy(d + AUTH2_SIZE, payload, payload_len);
    return d;
}

/* One EFI_SIGNATURE_LIST with `count` entries of `data_len` bytes each. */
static uint8_t *make_sig_list(EFI_GUID *type, const uint8_t *data, UINTN data_len, int count, UINTN *len) {
    UINTN sig_size = 16 + data_len;
    *len = 28 + sig_size * count;
    uint8_t *l;
    if (EFI_ERROR(BS->AllocatePool(EfiLoaderData, *len, (void **)&l))) return NULL;
    memcpy(l, type, 16);
    uint32_t v = (uint32_t)*len; memcpy(l + 16, &v, 4);   /* SignatureListSize   */
    v = 0;                       memcpy(l + 20, &v, 4);   /* SignatureHeaderSize */
    v = (uint32_t)sig_size;      memcpy(l + 24, &v, 4);   /* SignatureSize       */
    for (int i = 0; i < count; i++) {
        memcpy(l + 28 + i * sig_size, &LAB_OWNER, 16);
        memcpy(l + 28 + i * sig_size + 16, data + i * data_len, data_len);
    }
    return l;
}

static EFI_STATUS set_auth_var(const CHAR16 *name, EFI_GUID *guid, uint32_t extra_attrs,
                               const uint8_t *payload, UINTN payload_len) {
    UINTN total; uint8_t *d = make_auth_var(payload, payload_len, &total);
    if (!d) return EFI_INVALID_PARAMETER;
    EFI_STATUS s = gST->RuntimeServices->SetVariable((CHAR16 *)name, guid, AUTH_VAR_ATTRS | extra_attrs, total, d);
    BS->FreePool(d);
    return s;
}

static uint8_t read_u8_var(const CHAR16 *name) {
    uint8_t v = 0xFF; uint32_t attr; UINTN size = 1;
    if (EFI_ERROR(gST->RuntimeServices->GetVariable((CHAR16 *)name, &GLOBAL_VAR, &attr, &size, &v))) return 0xFF;
    return v;
}

static void show_mode(void) {
    uint8_t setup = read_u8_var(u"SetupMode"), sb = read_u8_var(u"SecureBoot");
    print(u"    SetupMode = "); print_dec(setup); print(setup == 1 ? u" (setup mode: no PK, nothing enforced)" : u" (user mode: PK enrolled)");
    print(u", SecureBoot = "); print_dec(sb); print(u"\r\n");
}

/* ---------- SMI counter from our SMM handler (Phase 6) ---------- */
static uint64_t smi_count(void) {
    EFI_GUID comm_guid = EFI_SMM_COMMUNICATION_PROTOCOL_GUID, g = LAB_SMM_COMM_GUID;
    EFI_SMM_COMMUNICATION_PROTOCOL *comm; static uint8_t *buf;
    if (EFI_ERROR(BS->LocateProtocol(&comm_guid, NULL, (void **)&comm))) return 0;
    if (!buf) { uint64_t p; if (EFI_ERROR(BS->AllocatePages(AllocateAnyPages, EfiRuntimeServicesData, 1, &p))) return 0; buf = (uint8_t *)(uintptr_t)p; }
    EFI_SMM_COMMUNICATE_HEADER *h = (EFI_SMM_COMMUNICATE_HEADER *)buf;
    LAB_SMM_MSG *m = (LAB_SMM_MSG *)(buf + sizeof *h);
    memset(buf, 0, sizeof *h + sizeof *m);
    h->HeaderGuid = g; h->MessageLength = sizeof *m; m->Command = LAB_CMD_PING;
    UINTN size = sizeof *h + sizeof *m;
    if (EFI_ERROR(comm->Communicate(comm, buf, &size)) || m->Magic != LAB_SMM_MAGIC) return 0;
    return m->TotalSmis;
}

/* ---------- running images ---------- */
static int run_image(const CHAR16 *label, const CHAR16 *path, int tamper) {
    void *dp = file_device_path(path);
    uint8_t *buf = NULL; UINTN size = 0;
    if (tamper) {
        if (EFI_ERROR(read_file(path, &buf, &size))) { print(u"    cannot read file\r\n"); return 0; }
        buf[size / 2] ^= 0x01;                      /* flip ONE bit in the middle of the program */
    }
    EFI_HANDLE h = NULL;
    EFI_STATUS s = BS->LoadImage(0, gSelf, dp, buf, size, &h);
    print(u"    "); print(label);
    if (s == EFI_SUCCESS) {
        print(u": ALLOWED\r\n");
        BS->StartImage(h, NULL, NULL);
    } else {
        print(u": REFUSED by the firmware ("); print(status_name(s)); print(u")\r\n");
        if (s == EFI_SECURITY_VIOLATION && h) BS->UnloadImage(h);
    }
    if (buf) BS->FreePool(buf);
    return s == EFI_SUCCESS;
}

EFI_STATUS EFIAPI efi_main(EFI_HANDLE self, EFI_SYSTEM_TABLE *st) {
    gST = st; BS = st->BootServices; gSelf = self;
    BS->SetWatchdogTimer(0, 0, 0, NULL);
    st->ConOut->ClearScreen(st->ConOut);
    print(u"=== Phase 10: Secure Boot ===\r\n\r\n");

    uint8_t *tmp; UINTN tmpn;
    read_file(u"\\EFI\\BOOT\\BOOTX64.EFI", &tmp, &tmpn);   /* also records the boot device */

    int setup_at_boot = read_u8_var(u"SetupMode") == 1;
    print(u"[1] Secure Boot state at power-on\r\n");
    show_mode();

    if (setup_at_boot) {
        print(u"\r\n[2] Setup mode: the firmware runs anything\r\n");
        run_image(u"untrusted.efi", u"\\EFI\\lab\\untrusted.efi", 0);

        print(u"\r\n[3] Enrolling keys (allowed without signatures only in setup mode)\r\n");
        uint8_t hashes[64]; uint8_t *f; UINTN n;
        if (EFI_ERROR(read_file(u"\\EFI\\BOOT\\BOOTX64.EFI", &f, &n)) || !authenticode_sha256(f, n, hashes)) {
            print(u"    cannot hash BOOTX64.EFI\r\n"); halt();
        }
        if (EFI_ERROR(read_file(u"\\EFI\\lab\\trusted.efi", &f, &n)) || !authenticode_sha256(f, n, hashes + 32)) {
            print(u"    cannot hash trusted.efi\r\n"); halt();
        }
        print(u"    db  += SHA-256 of BOOTX64.EFI (this app): "); print_hash8(hashes); print(u"\r\n");
        print(u"    db  += SHA-256 of trusted.efi:            "); print_hash8(hashes + 32); print(u"\r\n");
        UINTN len; uint8_t *list = make_sig_list(&CERT_SHA256, hashes, 32, 2, &len);
        EFI_STATUS s = set_auth_var(u"db", &IMAGE_SEC_DB, 0, list, len);
        print(u"    write db : "); print(status_name(s)); print(u"\r\n");

        uint8_t *cert; UINTN certlen;
        if (EFI_ERROR(read_file(u"\\EFI\\lab\\LabTestPK.der", &cert, &certlen))) { print(u"    no LabTestPK.der\r\n"); halt(); }
        list = make_sig_list(&CERT_X509, cert, certlen, 1, &len);
        s = set_auth_var(u"KEK", &GLOBAL_VAR, 0, list, len);
        print(u"    write KEK: "); print(status_name(s)); print(u" (test certificate, private key discarded)\r\n");
        s = set_auth_var(u"PK", &GLOBAL_VAR, 0, list, len);
        print(u"    write PK : "); print(status_name(s)); print(u" (same certificate)\r\n");
        show_mode();
    } else {
        print(u"    The firmware checked THIS app's hash against db before starting it.\r\n");
    }

    print(u"\r\n[4] Secure Boot enforced: what may run?\r\n");
    int t = run_image(u"trusted.efi           (hash in db)", u"\\EFI\\lab\\trusted.efi", 0);
    int u = run_image(u"untrusted.efi         (not in db)", u"\\EFI\\lab\\untrusted.efi", 0);
    int x = run_image(u"trusted.efi, 1 bit flipped     ", u"\\EFI\\lab\\trusted.efi", 1);

    print(u"\r\n[5] Ring 0 tries to change the rules (the variable driver in SMM decides)\r\n");
    uint64_t smi0 = smi_count();
    uint8_t *f2; UINTN n2; uint8_t bad[32];
    read_file(u"\\EFI\\lab\\untrusted.efi", &f2, &n2); authenticode_sha256(f2, n2, bad);
    UINTN len2; uint8_t *l2 = make_sig_list(&CERT_SHA256, bad, 32, 1, &len2);
    EFI_STATUS a = set_auth_var(u"db", &IMAGE_SEC_DB, EFI_VARIABLE_APPEND_WRITE, l2, len2);
    print(u"    add untrusted.efi's hash to db (unsigned): "); print(status_name(a)); print(u"\r\n");
    EFI_STATUS d = set_auth_var(u"PK", &GLOBAL_VAR, 0, NULL, 0);
    print(u"    delete PK to turn Secure Boot off (unsigned): "); print(status_name(d)); print(u"\r\n");
    uint64_t smi1 = smi_count();
    print(u"    SMIs while ring 0 tried: "); print_dec(smi1 - smi0 - 1); print(u"\r\n");

    int ok = t && !u && !x && EFI_ERROR(a) && EFI_ERROR(d);
    print(u"\r\nSummary: trusted ALLOWED, untrusted ");
    print(u ? u"ALLOWED" : u"REFUSED"); print(u", tampered "); print(x ? u"ALLOWED" : u"REFUSED");
    print(u", rule changes "); print((EFI_ERROR(a) && EFI_ERROR(d)) ? u"REFUSED" : u"ACCEPTED");
    print(ok ? u" -> Secure Boot works\r\n" : u" -> something is off\r\n");
    if (setup_at_boot) print(u"Boot again: next time the firmware will check this app before running it.\r\n");

    print(u"\r\nShutting down in 3 s...\r\n");
    BS->Stall(3000000);
    st->RuntimeServices->ResetSystem(EfiResetShutdown, EFI_SUCCESS, 0, NULL);
    halt();
}
