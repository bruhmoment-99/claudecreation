'use strict'
const test = require('node:test')
const assert = require('node:assert')
const fs = require('fs')
const os = require('os')
const path = require('path')
const { loadSchematic, parseStateString, unpackLitematicStates } = require('../src/schematic')
const { writeSchem, writeLitematic } = require('../src/writers')

const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'cannonbot-'))
const sortKey = b => `${b.y},${b.z},${b.x}`
const norm = list => list.map(b => ({ x: b.x, y: b.y, z: b.z, name: b.name, props: b.props })).sort((a, b) => sortKey(a) < sortKey(b) ? -1 : 1)

// Enough distinct states to need 6 bits, so litematic entries straddle 64-bit words
function sampleBlocks () {
  const blocks = []
  const dirs = ['north', 'south', 'east', 'west']
  let i = 0
  for (let x = 0; x < 6; x++) {
    for (let y = 0; y < 4; y++) {
      for (let z = 0; z < 5; z++) {
        if ((x + y + z) % 3 === 0) continue // leave some air
        const kind = i++ % 4
        if (kind === 0) blocks.push({ x, y, z, name: 'stone', props: {} })
        else if (kind === 1) blocks.push({ x, y, z, name: 'repeater', props: { delay: String(1 + (i % 4)), facing: dirs[i % 4], locked: 'false', powered: 'false' } })
        else if (kind === 2) blocks.push({ x, y, z, name: 'observer', props: { facing: dirs[(i + 1) % 4], powered: 'false' } })
        else blocks.push({ x, y, z, name: 'note_block', props: { instrument: 'harp', note: String(i % 25), powered: 'false' } })
      }
    }
  }
  return blocks
}

test('parses block state strings', () => {
  assert.deepStrictEqual(parseStateString('minecraft:repeater[delay=2,facing=north]'), { name: 'repeater', props: { delay: '2', facing: 'north' } })
  assert.deepStrictEqual(parseStateString('stone'), { name: 'stone', props: {} })
})

test('round-trips a Sponge .schem file', async () => {
  const blocks = sampleBlocks()
  const file = path.join(tmp, 'a.schem')
  fs.writeFileSync(file, writeSchem(blocks))
  const loaded = await loadSchematic(file)
  assert.deepStrictEqual(loaded.size, { x: 6, y: 4, z: 5 })
  assert.deepStrictEqual(norm(loaded.blocks), norm(blocks))
})

test('round-trips a .litematic file with entries spanning two longs', async () => {
  const blocks = sampleBlocks()
  const file = path.join(tmp, 'a.litematic')
  fs.writeFileSync(file, writeLitematic(blocks, { offset: { x: 10, y: -3, z: 7 } }))
  const loaded = await loadSchematic(file)
  assert.deepStrictEqual(norm(loaded.blocks), norm(blocks))
})

test('handles litematic regions with negative sizes', async () => {
  const blocks = sampleBlocks()
  const file = path.join(tmp, 'neg.litematic')
  fs.writeFileSync(file, writeLitematic(blocks, { negativeSize: true }))
  const loaded = await loadSchematic(file)
  assert.deepStrictEqual(norm(loaded.blocks), norm(blocks))
})

test('unpacks litematic bit-packed arrays', () => {
  // 3 bits per entry, values 0..7 repeated; entry 21 straddles the first two longs
  const values = Array.from({ length: 40 }, (_, i) => i % 8)
  let a = 0n; let b = 0n
  values.forEach((v, i) => {
    const bit = i * 3
    if (bit < 64) a |= BigInt(v) << BigInt(bit)
    if (bit + 3 > 64) b |= BigInt(v) >> BigInt(64 - bit) >> 0n
    if (bit >= 64) b |= BigInt(v) << BigInt(bit - 64)
  })
  a = BigInt.asUintN(64, a)
  const toPair = w => { const s = BigInt.asIntN(64, w); return [Number(BigInt.asIntN(32, s >> 32n)), Number(BigInt.asIntN(32, s & 0xffffffffn))] }
  assert.deepStrictEqual(unpackLitematicStates([toPair(a), toPair(b)], 3, 40), values)
})

test('rejects legacy .schematic files with a helpful message', async () => {
  const file = path.join(tmp, 'old.schematic')
  const nbt = require('prismarine-nbt')
  fs.writeFileSync(file, nbt.writeUncompressed(nbt.comp({ Width: nbt.short(1) }, 'Schematic')))
  await assert.rejects(loadSchematic(file), /not supported/)
})
