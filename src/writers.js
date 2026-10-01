'use strict'
// Writes .schem / .litematic files. Used for the demo build and the tests.

const zlib = require('zlib')
const nbt = require('prismarine-nbt')

function stateString ({ name, props }) {
  const keys = Object.keys(props || {})
  return `minecraft:${name}` + (keys.length ? `[${keys.sort().map(k => `${k}=${props[k]}`).join(',')}]` : '')
}

function bounds (blocks) {
  const size = { x: 0, y: 0, z: 0 }
  for (const b of blocks) {
    size.x = Math.max(size.x, b.x + 1); size.y = Math.max(size.y, b.y + 1); size.z = Math.max(size.z, b.z + 1)
  }
  return size
}

function grid (blocks, size) {
  const cells = new Array(size.x * size.y * size.z).fill('minecraft:air')
  for (const b of blocks) cells[(b.y * size.z + b.z) * size.x + b.x] = stateString(b)
  return cells
}

// Sponge schematic v2
function writeSchem (blocks) {
  const size = bounds(blocks)
  const cells = grid(blocks, size)
  const palette = {}
  const data = []
  for (const s of cells) {
    if (!(s in palette)) palette[s] = Object.keys(palette).length
    let v = palette[s]
    do {
      let byte = v & 0x7f
      v >>>= 7
      if (v) byte |= 0x80
      data.push(byte > 127 ? byte - 256 : byte)
    } while (v)
  }
  const paletteTag = {}
  for (const [k, v] of Object.entries(palette)) paletteTag[k] = nbt.int(v)
  const root = nbt.comp({
    Version: nbt.int(2),
    DataVersion: nbt.int(3953),
    Width: nbt.short(size.x),
    Height: nbt.short(size.y),
    Length: nbt.short(size.z),
    PaletteMax: nbt.int(Object.keys(palette).length),
    Palette: nbt.comp(paletteTag),
    BlockData: nbt.byteArray(data)
  }, 'Schematic')
  return zlib.gzipSync(nbt.writeUncompressed(root))
}

function writeLitematic (blocks, { offset = { x: 0, y: 0, z: 0 }, negativeSize = false } = {}) {
  const size = bounds(blocks)
  const cells = grid(blocks, size)
  const palette = ['minecraft:air']
  const ids = cells.map(s => {
    let i = palette.indexOf(s)
    if (i < 0) { palette.push(s); i = palette.length - 1 }
    return i
  })
  const bits = Math.max(2, Math.ceil(Math.log2(palette.length)))
  const longCount = Math.ceil(ids.length * bits / 64)
  const words = new Array(longCount).fill(0n)
  ids.forEach((id, i) => {
    const start = i * bits
    const li = Math.floor(start / 64)
    const off = BigInt(start % 64)
    words[li] |= (BigInt(id) << off) & ((1n << 64n) - 1n)
    if (Math.floor((start + bits - 1) / 64) !== li) words[li + 1] |= BigInt(id) >> (64n - off)
  })
  const longs = words.map(w => {
    const signed = BigInt.asIntN(64, w)
    return [Number(BigInt.asIntN(32, signed >> 32n)), Number(BigInt.asIntN(32, signed & 0xffffffffn))]
  })
  const paletteList = palette.map(s => {
    const m = /^minecraft:([^[]+)(?:\[(.*)\])?$/.exec(s)
    const tag = { Name: nbt.string(`minecraft:${m[1]}`) }
    if (m[2]) {
      const props = {}
      for (const pair of m[2].split(',')) { const [k, v] = pair.split('='); props[k] = nbt.string(v) }
      tag.Properties = nbt.comp(props)
    }
    return tag
  })
  // A negative size means Position is the far corner and the region extends backwards
  const pos = negativeSize
    ? { x: offset.x + size.x - 1, y: offset.y + size.y - 1, z: offset.z + size.z - 1 }
    : offset
  const sz = negativeSize ? { x: -size.x, y: -size.y, z: -size.z } : size
  const vec3 = v => nbt.comp({ x: nbt.int(v.x), y: nbt.int(v.y), z: nbt.int(v.z) })
  const root = nbt.comp({
    Version: nbt.int(6),
    MinecraftDataVersion: nbt.int(3953),
    Regions: nbt.comp({
      main: nbt.comp({
        Position: vec3(pos),
        Size: vec3(sz),
        BlockStatePalette: nbt.list(nbt.comp(paletteList)),
        BlockStates: nbt.longArray(longs)
      })
    })
  })
  return zlib.gzipSync(nbt.writeUncompressed(root))
}

module.exports = { writeSchem, writeLitematic, stateString }
