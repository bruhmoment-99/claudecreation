'use strict'
const test = require('node:test')
const assert = require('node:assert')
const { parseCommand } = require('../src/commands')
const { dps } = require('../src/combat')

test('build at absolute coordinates', () => {
  assert.deepStrictEqual(parseCommand('build at 120 64 -300', null), { type: 'build', origin: { x: 120, y: 64, z: -300 } })
})

test('build at relative coordinates and with rotation', () => {
  const me = { x: 10.7, y: 70, z: -5.2 }
  assert.deepStrictEqual(parseCommand('build at ~5 ~ ~-10 rotate 90', me), { type: 'build', origin: { x: 15, y: 70, z: -16 }, rotate: 90 })
})

test('bad build commands explain themselves', () => {
  assert.strictEqual(parseCommand('build at 1 2', null).type, 'error')
  assert.strictEqual(parseCommand('build at ~ ~ ~', null).type, 'error') // no position to be relative to
  assert.strictEqual(parseCommand('build at 0 999 0', null).type, 'error')
  assert.strictEqual(parseCommand('build at 0 64 0 rotate 45', null).type, 'error')
  assert.strictEqual(parseCommand('build here', null).type, 'error')
})

test('build here uses the speaker position', () => {
  assert.deepStrictEqual(parseCommand('build here', { x: 1.5, y: 64, z: -0.5 }), { type: 'build', origin: { x: 1, y: 64, z: -1 } })
})

test('other commands', () => {
  assert.deepStrictEqual(parseCommand('hunt Steve', null), { type: 'hunt', player: 'Steve' })
  assert.strictEqual(parseCommand('hunt', null).type, 'error')
  assert.deepStrictEqual(parseCommand('GUARD', null), { type: 'guard' })
  assert.deepStrictEqual(parseCommand('stop', null), { type: 'stop' })
  assert.strictEqual(parseCommand('hello there', null), null) // ordinary chat is ignored
  assert.strictEqual(parseCommand('stop it please', null), null)
})

test('prefers a sword over a slower axe', () => {
  assert.ok(dps('diamond_sword') > dps('diamond_axe'))
  assert.ok(dps('netherite_sword') > dps('iron_sword'))
})
