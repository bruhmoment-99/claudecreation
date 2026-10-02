'use strict'
// Sets up a Mindcraft (https://github.com/kolbytn/mindcraft) bot to gather and craft
// the build's materials and drop them in the supply chest, where CannonBot picks
// them up. Mindcraft is a separate project: this writes its profile/settings and,
// if config.mindcraft.path points at your Mindcraft folder, starts it.

const fs = require('fs')
const path = require('path')
const { spawn } = require('child_process')

const OUT_DIR = 'mindcraft-setup'

function dropOffPoint (cfg) {
  const m = cfg.mindcraft || {}
  if (Array.isArray(m.dropOff)) return m.dropOff
  if (Array.isArray(cfg.chests) && cfg.chests.length) return cfg.chests[0]
  return null
}

// The shopping list, biggest first, without anything the build skips (e.g. TNT)
function shoppingList (plan, cfg) {
  const skip = new Set(cfg.skipBlocks || [])
  return Object.entries(plan.materials)
    .filter(([item]) => !skip.has(item))
    .sort((a, b) => b[1] - a[1])
}

function goalText (plan, cfg) {
  const [x, y, z] = dropOffPoint(cfg)
  const items = shoppingList(plan, cfg).map(([item, n]) => `${n} ${item}`).join(', ')
  let area = ''
  if (Array.isArray(cfg.origin)) {
    const [ox, oy, oz] = cfg.origin
    area = ` Do not dig or build anywhere between x ${ox} to ${ox + plan.size.x - 1}, y ${oy} to ${oy + plan.size.y - 1}, z ${oz} to ${oz + plan.size.z - 1} - that is the build site.`
  }
  return `Collect or craft these items for a build: ${items}. ` +
    'Get the right tools first, then gather raw materials and craft the finished items. ' +
    `Whenever your inventory is getting full, and when you are done, go to the chest at ${x} ${y} ${z} with !goToCoordinates and store the items with !putInChest. ` +
    `Keep your tools and food. Only take blocks from natural terrain and never break anything players have built.${area} ` +
    'When everything has been delivered, use !endGoal.'
}

function profile (cfg) {
  const m = cfg.mindcraft || {}
  return {
    name: m.username || 'Gatherer',
    model: m.model || {
      api: 'anthropic',
      model: 'claude-opus-5-5',
      // Mindcraft defaults to 4096 tokens, which thinking can use up before the reply
      params: { max_tokens: 16000, output_config: { effort: 'low' } }
    }
  }
}

function settings (cfg, goal) {
  const m = cfg.mindcraft || {}
  return {
    host: cfg.host || 'localhost',
    port: cfg.port || 25565,
    auth: m.auth || cfg.auth || 'offline',
    minecraft_version: cfg.version || 'auto',
    base_profile: 'survival',
    load_memory: true, // pick up where it left off after a restart
    only_chat_with: cfg.owner ? [cfg.owner] : [],
    init_message: `Use the !goal command right away with this goal: ${goal}`,
    ...(m.settings || {})
  }
}

async function run (cfg, plan) {
  const m = cfg.mindcraft || {}
  if (!dropOffPoint(cfg)) {
    throw new Error('Tell me where the gatherer should drop things off: set "mindcraft": { "dropOff": [x, y, z] } (a chest next to the build) or list a chest in "chests".')
  }
  const name = profile(cfg).name
  if (name.toLowerCase() === String(cfg.username || 'CannonBot').toLowerCase()) {
    throw new Error('The Mindcraft bot needs a different username from CannonBot - set "mindcraft": { "username": "..." }.')
  }

  const goal = goalText(plan, cfg)
  const dir = path.resolve(OUT_DIR)
  fs.mkdirSync(dir, { recursive: true })
  const profilePath = path.join(dir, `${name}.json`)
  const settingsJson = JSON.stringify(settings(cfg, goal))
  fs.writeFileSync(profilePath, JSON.stringify(profile(cfg), null, 2) + '\n')
  fs.writeFileSync(path.join(dir, 'settings.json'), JSON.stringify(settings(cfg, goal), null, 2) + '\n')
  fs.writeFileSync(path.join(dir, 'goal.txt'), goal + '\n')

  console.log(`Wrote ${OUT_DIR}/${name}.json, settings.json and goal.txt\n`)
  console.log('Shopping list for the gatherer:')
  for (const [item, n] of shoppingList(plan, cfg)) console.log(`  ${String(n).padStart(6)}  ${item}`)
  console.log('')

  if (!m.path) {
    console.log('To start it, set "mindcraft": { "path": "<your mindcraft folder>" } in your config and run this again,')
    console.log('or start Mindcraft yourself from its folder with these environment variables:')
    console.log(`  PROFILES='${JSON.stringify([profilePath])}'`)
    console.log(`  SETTINGS_JSON='<contents of ${OUT_DIR}/settings.json>'`)
    return
  }

  const mcDir = path.resolve(m.path)
  if (!fs.existsSync(path.join(mcDir, 'main.js'))) throw new Error(`No Mindcraft main.js in ${mcDir}`)
  console.log(`Starting Mindcraft in ${mcDir} as ${name}...`)
  const child = spawn(process.execPath, ['main.js'], {
    cwd: mcDir,
    stdio: 'inherit',
    env: { ...process.env, PROFILES: JSON.stringify([profilePath]), SETTINGS_JSON: settingsJson }
  })
  await new Promise(resolve => child.on('exit', resolve))
}

module.exports = { run, goalText, profile, settings, shoppingList }
