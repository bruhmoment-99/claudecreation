'use strict'
// Builds examples/demo.schem: a small redstone test piece that exercises the tricky
// placement cases (repeaters with delays, observer, sticky piston, hopper, stairs,
// slabs, wall lever, comparator). Build this first to check the bot works on your
// server before trusting it with a big cannon.

const fs = require('fs')
const path = require('path')
const { writeSchem } = require('../src/writers')

const blocks = []
const add = (x, y, z, name, props = {}) => blocks.push({ x, y, z, name, props })

// 7x7 stone floor
for (let x = 0; x < 7; x++) for (let z = 0; z < 7; z++) add(x, 0, z, 'smooth_stone')

// lever on a block -> repeater chain -> redstone lamp
add(0, 1, 3, 'stone_bricks')
add(1, 1, 3, 'lever', { face: 'wall', facing: 'east', powered: 'false' })
add(2, 1, 3, 'repeater', { facing: 'west', delay: '3', locked: 'false', powered: 'false' })
add(3, 1, 3, 'redstone_wire', { east: 'side', west: 'side', north: 'none', south: 'none', power: '0' })
add(4, 1, 3, 'repeater', { facing: 'west', delay: '2', locked: 'false', powered: 'false' })
add(5, 1, 3, 'redstone_lamp', { lit: 'false' })

// observer watching north, sticky piston pushing up, hopper into a chest
add(1, 1, 1, 'observer', { facing: 'north', powered: 'false' })
add(3, 1, 1, 'sticky_piston', { facing: 'up', extended: 'false' })
add(5, 1, 1, 'chest', { facing: 'west', type: 'single', waterlogged: 'false' })
add(5, 2, 1, 'hopper', { facing: 'down', enabled: 'true' })

// stairs, slabs, comparator
add(1, 1, 5, 'stone_brick_stairs', { facing: 'east', half: 'bottom', shape: 'straight', waterlogged: 'false' })
add(2, 1, 5, 'stone_brick_stairs', { facing: 'south', half: 'top', shape: 'straight', waterlogged: 'false' })
add(3, 1, 5, 'smooth_stone_slab', { type: 'bottom', waterlogged: 'false' })
add(4, 1, 5, 'smooth_stone_slab', { type: 'top', waterlogged: 'false' })
add(5, 1, 5, 'comparator', { facing: 'north', mode: 'subtract', powered: 'false' })

const out = path.join(__dirname, '..', 'examples', 'demo.schem')
fs.mkdirSync(path.dirname(out), { recursive: true })
fs.writeFileSync(out, writeSchem(blocks))
console.log(`Wrote ${out} (${blocks.length} blocks)`)
