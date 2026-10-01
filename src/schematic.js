'use strict'
// Loads Sponge (.schem) and Litematica (.litematic) files into a flat list of
// blocks: { x, y, z, name, props } with coordinates relative to the schematic's
// minimum corner. Air is dropped.

const fs = require('fs')
const path = require('path')
const nbt = require('prismarine-nbt')

const AIR = new Set(['air', 'cave_air', 'void_air', 'structure_void'])

// "minecraft:repeater[delay=2,facing=north]" -> { name: 'repeater', props: { delay: '2', facing: 'north' } }
function parseStateString (str) {
  const m = /^(?:([a-z0-9_.-]+):)?([a-z0-9_/.-]+)(?:\[(.*)\])?$/.exec(str.trim())
  if (!m) throw new Error(`Unrecognised block state "${str}"`)
  const props = {}
  if (m[3]) {
    for (const pair of m[3].split(',')) {
      if (!pair) continue
      const [k, v] = pair.split('=')
      props[k.trim()] = String(v).trim()
    }
  }
  return { name: m[2], props }
}

function stringifyProps (props) {
  const out = {}
  for (const [k, v] of Object.entries(props || {})) out[k] = String(v)
  return out
}

function readVarInts (bytes, count) {
  const out = new Array(count)
  let i = 0
  for (let n = 0; n < count; n++) {
    let value = 0
    let shift = 0
    let b
    do {
      if (i >= bytes.length) throw new Error('Block data ended early - the .schem file looks corrupt')
      b = bytes[i++] & 0xff
      value |= (b & 0x7f) << shift
      shift += 7
    } while (b & 0x80)
    out[n] = value
  }
  return out
}

function loadSponge (root) {
  // v3 nests everything under "Schematic" and block data under "Blocks"
  const s = root.Schematic || root
  const width = s.Width
  const height = s.Height
  const length = s.Length
  const blocksTag = s.Blocks || s
  const palette = blocksTag.Palette
  const data = blocksTag.Data || blocksTag.BlockData
  if (!palette || !data) throw new Error('This .schem file has no block palette/data')

  const byId = []
  for (const [state, id] of Object.entries(palette)) byId[id] = parseStateString(state)

  const ids = readVarInts(data, width * height * length)
  const blocks = []
  for (let y = 0; y < height; y++) {
    for (let z = 0; z < length; z++) {
      for (let x = 0; x < width; x++) {
        const state = byId[ids[(y * length + z) * width + x]]
        if (!state || AIR.has(state.name)) continue
        blocks.push({ x, y, z, name: state.name, props: { ...state.props } })
      }
    }
  }
  return { size: { x: width, y: height, z: length }, blocks }
}

function toUint64 ([hi, lo]) {
  return BigInt.asUintN(64, (BigInt(hi) << 32n) | BigInt(lo >>> 0))
}

// Litematica packs palette indices tightly; an entry may straddle two longs.
function unpackLitematicStates (longs, bits, count) {
  const mask = (1n << BigInt(bits)) - 1n
  const words = longs.map(toUint64)
  const out = new Array(count)
  for (let i = 0; i < count; i++) {
    const startBit = i * bits
    const startLong = Math.floor(startBit / 64)
    const endLong = Math.floor((startBit + bits - 1) / 64)
    const offset = BigInt(startBit % 64)
    let v = words[startLong] >> offset
    if (endLong !== startLong) v |= words[endLong] << (64n - offset)
    out[i] = Number(v & mask)
  }
  return out
}

function loadLitematic (root) {
  if (!root.Regions) throw new Error('This .litematic file has no regions')
  const blocks = []
  for (const [regionName, region] of Object.entries(root.Regions)) {
    const size = region.Size
    const pos = region.Position
    const sx = Math.abs(size.x)
    const sy = Math.abs(size.y)
    const sz = Math.abs(size.z)
    // Negative sizes mean the region extends in the negative direction from Position
    const minX = pos.x + (size.x < 0 ? size.x + 1 : 0)
    const minY = pos.y + (size.y < 0 ? size.y + 1 : 0)
    const minZ = pos.z + (size.z < 0 ? size.z + 1 : 0)
    const palette = (region.BlockStatePalette || []).map(p => ({
      name: p.Name.replace(/^[a-z0-9_.-]+:/, ''),
      props: stringifyProps(p.Properties)
    }))
    const bits = Math.max(2, Math.ceil(Math.log2(palette.length)))
    const count = sx * sy * sz
    if (!region.BlockStates) throw new Error(`Region "${regionName}" has no block data`)
    const ids = unpackLitematicStates(region.BlockStates, bits, count)
    for (let y = 0; y < sy; y++) {
      for (let z = 0; z < sz; z++) {
        for (let x = 0; x < sx; x++) {
          const state = palette[ids[(y * sz + z) * sx + x]]
          if (!state || AIR.has(state.name)) continue
          blocks.push({ x: minX + x, y: minY + y, z: minZ + z, name: state.name, props: { ...state.props } })
        }
      }
    }
  }
  return normalise(blocks)
}

// Shift so the minimum corner is at 0,0,0
function normalise (blocks) {
  if (blocks.length === 0) return { size: { x: 0, y: 0, z: 0 }, blocks }
  let min = { x: Infinity, y: Infinity, z: Infinity }
  let max = { x: -Infinity, y: -Infinity, z: -Infinity }
  for (const b of blocks) {
    min = { x: Math.min(min.x, b.x), y: Math.min(min.y, b.y), z: Math.min(min.z, b.z) }
    max = { x: Math.max(max.x, b.x), y: Math.max(max.y, b.y), z: Math.max(max.z, b.z) }
  }
  for (const b of blocks) { b.x -= min.x; b.y -= min.y; b.z -= min.z }
  return { size: { x: max.x - min.x + 1, y: max.y - min.y + 1, z: max.z - min.z + 1 }, blocks }
}

async function loadSchematic (file) {
  const buf = fs.readFileSync(file)
  const { parsed } = await nbt.parse(buf) // handles gzip transparently
  const root = nbt.simplify(parsed)
  const ext = path.extname(file).toLowerCase()
  if (ext === '.litematic' || root.Regions) return loadLitematic(root)
  if (ext === '.schem' || root.Palette || root.Schematic) return loadSponge(root)
  if (ext === '.schematic') {
    throw new Error('Old MCEdit .schematic files (pre-1.13) are not supported. Open it in WorldEdit or Litematica and save it as .schem or .litematic.')
  }
  throw new Error(`Don't know how to read ${path.basename(file)} - use a .schem or .litematic file`)
}

module.exports = { loadSchematic, parseStateString, unpackLitematicStates, AIR }
