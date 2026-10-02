'use strict'
const test = require('node:test')
const assert = require('node:assert')
const { goalText, profile, settings, shoppingList } = require('../src/mindcraft')

const plan = { size: { x: 3, y: 2, z: 4 }, materials: { obsidian: 40, tnt: 12, repeater: 3, water_bucket: 1 } }
const cfg = { host: 'example.net', port: 25570, owner: 'Me', origin: [100, 64, -20], chests: [[98, 64, -22]], skipBlocks: ['tnt'] }

test('shopping list skips skipBlocks and sorts biggest first', () => {
  assert.deepStrictEqual(shoppingList(plan, cfg), [['obsidian', 40], ['repeater', 3], ['water_bucket', 1]])
})

test('goal names the items, the drop-off chest and the build site to avoid', () => {
  const goal = goalText(plan, cfg)
  assert.match(goal, /40 obsidian, 3 repeater, 1 water_bucket/)
  assert.doesNotMatch(goal, /tnt/)
  assert.match(goal, /chest at 98 64 -22/)
  assert.match(goal, /x 100 to 102, y 64 to 65, z -20 to -17/)
  assert.match(goal, /never break anything players have built/)
})

test('profile and settings point Mindcraft at the same server', () => {
  assert.strictEqual(profile(cfg).name, 'Gatherer')
  assert.strictEqual(profile(cfg).model.model, 'claude-opus-5-5')
  const s = settings(cfg, 'GOAL')
  assert.strictEqual(s.host, 'example.net')
  assert.strictEqual(s.port, 25570)
  assert.deepStrictEqual(s.only_chat_with, ['Me'])
  assert.match(s.init_message, /!goal.*GOAL/)
})

test('config can override the model and username', () => {
  const p = profile({ mindcraft: { username: 'Digger', model: 'claude-sonnet-5-5' } })
  assert.deepStrictEqual(p, { name: 'Digger', model: 'claude-sonnet-5-5' })
})
