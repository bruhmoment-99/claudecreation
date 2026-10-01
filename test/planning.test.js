'use strict'
const test = require('node:test')
const assert = require('node:assert')
const { placementFor, itemNameFor, lookAngles } = require('../src/placement')
const { makePlan } = require('../src/planner')
const { rotateSchematic } = require('../src/transform')
const registry = require('minecraft-data')('1.21.4')

test('item names for blocks that drop something else', () => {
  assert.strictEqual(itemNameFor('redstone_wire'), 'redstone')
  assert.strictEqual(itemNameFor('redstone_wall_torch'), 'redstone_torch')
  assert.strictEqual(itemNameFor('oak_wall_sign'), 'oak_sign')
  assert.strictEqual(itemNameFor('tnt'), 'tnt')
  // every override must be a real item
  for (const name of ['redstone_wire', 'wall_torch', 'water', 'lava', 'redstone_wall_torch']) {
    assert.ok(registry.itemsByName[itemNameFor(name)], name)
  }
})

test('repeaters: placed on the floor, looking against their facing, delay set by clicking', () => {
  const p = placementFor('repeater', { facing: 'west', delay: '3' })
  assert.deepStrictEqual(p.attempts[0], { refDirs: ['down'], look: 'east' })
  assert.strictEqual(p.attempts[1].look, 'west') // fallback if the server disagrees
  assert.strictEqual(p.clicks, 2)
  assert.deepStrictEqual(p.check, ['facing', 'delay'])
})

test('observers look the way they face, pistons look back at the player', () => {
  assert.strictEqual(placementFor('observer', { facing: 'up' }).attempts[0].look, 'up')
  assert.strictEqual(placementFor('sticky_piston', { facing: 'up' }).attempts[0].look, 'down')
  assert.strictEqual(placementFor('dispenser', { facing: 'north' }).attempts[0].look, 'south')
})

test('wall-mounted things are clicked onto the block behind them', () => {
  assert.deepStrictEqual(placementFor('lever', { face: 'wall', facing: 'east' }).attempts[0].refDirs, ['west'])
  assert.deepStrictEqual(placementFor('redstone_wall_torch', { facing: 'north' }).attempts[0].refDirs, ['south'])
  assert.deepStrictEqual(placementFor('stone_button', { face: 'floor', facing: 'east' }).attempts[0].refDirs, ['down'])
})

test('hoppers point into the block they are clicked against', () => {
  assert.deepStrictEqual(placementFor('hopper', { facing: 'east' }).attempts[0].refDirs, ['east'])
  assert.deepStrictEqual(placementFor('hopper', { facing: 'down' }).attempts[0].refDirs, ['down', 'up'])
})

test('slabs, stairs and pillars', () => {
  assert.deepStrictEqual(placementFor('oak_slab', { type: 'top' }).attempts[0].refDirs, ['up'])
  assert.strictEqual(placementFor('oak_slab', { type: 'double' }).doubleSlab, true)
  assert.strictEqual(placementFor('stone_stairs', { facing: 'south', half: 'top' }).attempts[1].half, 'top')
  assert.deepStrictEqual(placementFor('oak_log', { axis: 'x' }).attempts[0].refDirs, ['west', 'east'])
})

test('skips blocks that are not placed directly', () => {
  assert.ok(placementFor('piston_head', { facing: 'up' }).skip)
  assert.ok(placementFor('water', { level: '3' }).skip)
  assert.ok(placementFor('iron_door', { half: 'upper', facing: 'north' }).skip)
  assert.ok(!placementFor('water', { level: '0' }).skip)
})

test('look angles follow mineflayer conventions', () => {
  assert.strictEqual(lookAngles('north').yaw, 0)
  assert.strictEqual(lookAngles('west').yaw, Math.PI / 2)
  assert.ok(lookAngles('up').pitch > 1.5)
  assert.strictEqual(lookAngles(null), null)
})

test('plan orders structure first, TNT last, bottom-up, and counts materials', () => {
  const schem = {
    size: { x: 3, y: 2, z: 1 },
    blocks: [
      { x: 0, y: 1, z: 0, name: 'tnt', props: { unstable: 'false' } },
      { x: 1, y: 1, z: 0, name: 'repeater', props: { facing: 'east', delay: '1' } },
      { x: 0, y: 0, z: 0, name: 'obsidian', props: {} },
      { x: 1, y: 0, z: 0, name: 'obsidian', props: {} },
      { x: 2, y: 0, z: 0, name: 'water', props: { level: '0' } },
      { x: 2, y: 1, z: 0, name: 'water', props: { level: '0' } },
      { x: 2, y: 1, z: 0, name: 'piston_head', props: { facing: 'up' } },
      { x: 2, y: 1, z: 0, name: 'made_up_block', props: {} }
    ]
  }
  const plan = makePlan(schem, registry)
  assert.deepStrictEqual(plan.steps.map(s => s.name), ['obsidian', 'obsidian', 'repeater', 'water', 'water', 'tnt'])
  assert.deepStrictEqual(plan.materials, { obsidian: 2, repeater: 1, tnt: 1, water_bucket: 1 })
  assert.deepStrictEqual(plan.unknown, ['made_up_block'])
  assert.ok(Object.keys(plan.skipped).some(k => k.startsWith('piston_head')))
})

test('skipBlocks leaves blocks out', () => {
  const plan = makePlan({ size: { x: 1, y: 1, z: 1 }, blocks: [{ x: 0, y: 0, z: 0, name: 'tnt', props: {} }] }, registry, { skipBlocks: ['tnt'] })
  assert.strictEqual(plan.steps.length, 0)
})

test('rotation moves blocks and turns their facing', () => {
  const schem = {
    size: { x: 3, y: 1, z: 2 },
    blocks: [
      { x: 0, y: 0, z: 0, name: 'repeater', props: { facing: 'north', delay: '1' } },
      { x: 2, y: 0, z: 1, name: 'oak_log', props: { axis: 'x' } },
      { x: 1, y: 0, z: 0, name: 'rail', props: { shape: 'north_east' } },
      { x: 1, y: 0, z: 1, name: 'redstone_wire', props: { north: 'side', east: 'none', south: 'up', west: 'none' } }
    ]
  }
  const r = rotateSchematic(schem, 90)
  assert.deepStrictEqual(r.size, { x: 2, y: 1, z: 3 })
  const at = (x, z) => r.blocks.find(b => b.x === x && b.z === z)
  assert.strictEqual(at(1, 0).props.facing, 'east') // (0,0) -> (sizeZ-1-0, 0)
  assert.strictEqual(at(0, 2).props.axis, 'z')
  assert.strictEqual(at(1, 1).props.shape, 'south_east')
  assert.deepStrictEqual(at(0, 1).props, { east: 'side', south: 'none', west: 'up', north: 'none' })
  // four quarter turns is a no-op
  let back = schem
  for (let i = 0; i < 4; i++) back = rotateSchematic(back, 90)
  assert.deepStrictEqual(back, schem)
})
