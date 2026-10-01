#!/usr/bin/env node
'use strict'

const fs = require('fs')
const path = require('path')
const readline = require('readline')
const { loadSchematic } = require('./schematic')
const { rotateSchematic } = require('./transform')
const { makePlan, formatMaterials } = require('./planner')
const { PHASES } = require('./placement')

const USAGE = `
Usage:
  node src/index.js plan  [config.json]   show the material list and build order (no Minecraft needed)
  node src/index.js build [config.json]   join the server and build it

Copy config.example.json to config.json and fill it in first. See README.md.
`

function loadConfig (file) {
  const p = path.resolve(file)
  if (!fs.existsSync(p)) {
    console.error(`Can't find ${file}. Copy config.example.json to config.json and edit it.`)
    process.exit(1)
  }
  const cfg = JSON.parse(fs.readFileSync(p, 'utf8'))
  if (!cfg.schematic) throw new Error('config: "schematic" (path to your .litematic or .schem file) is required')
  cfg.schematic = path.resolve(path.dirname(p), cfg.schematic)
  return cfg
}

async function loadPlan (cfg, registry) {
  let schem = await loadSchematic(cfg.schematic)
  if (cfg.rotate) schem = rotateSchematic(schem, cfg.rotate)
  return makePlan(schem, registry, { skipBlocks: cfg.skipBlocks })
}

function printPlan (plan) {
  const s = plan.size
  console.log(`Size: ${s.x} wide (x) x ${s.y} tall (y) x ${s.z} long (z), ${plan.steps.length} blocks to place\n`)
  console.log('Materials:')
  console.log(formatMaterials(plan.materials, null))
  for (const [fluid, n] of Object.entries(plan.fluidSources || {})) {
    console.log(`  (${n} ${fluid} source blocks - one ${fluid}_bucket is enough if there's ${fluid} within 64 blocks to refill from)`)
  }
  console.log('\nBuild order:')
  for (const phase of PHASES) {
    const n = plan.steps.filter(st => st.how.category === phase).length
    if (n) console.log(`  ${phase.padEnd(10)} ${n}`)
  }
  if (Object.keys(plan.skipped).length) {
    console.log('\nNot placed (on purpose):')
    for (const [k, n] of Object.entries(plan.skipped)) console.log(`  ${String(n).padStart(6)}  ${k}`)
  }
  if (plan.unknown.length) {
    console.log(`\nWARNING: these blocks don't exist in this Minecraft version and will be skipped: ${plan.unknown.join(', ')}`)
  }
}

async function cmdPlan (cfg) {
  const registry = require('minecraft-data')(cfg.version || '1.21.4')
  if (!registry) throw new Error(`Unknown Minecraft version "${cfg.version}"`)
  printPlan(await loadPlan(cfg, registry))
}

async function cmdBuild (cfg) {
  const mineflayer = require('mineflayer')
  const { pathfinder } = require('mineflayer-pathfinder')
  const { Vec3 } = require('vec3')
  const { Builder } = require('./builder')

  const log = (...a) => console.log(`[${new Date().toLocaleTimeString()}]`, ...a)

  const bot = mineflayer.createBot({
    host: cfg.host || 'localhost',
    port: cfg.port || 25565,
    username: cfg.username || 'CannonBot',
    auth: cfg.auth || 'offline',
    version: cfg.version || false
  })
  bot.loadPlugin(pathfinder)

  let builder = null

  const start = async (origin) => {
    if (builder.state === 'building' || builder.state === 'paused') {
      return builder.say(`Already ${builder.state}. Say "stop" first if you want to move the build.`)
    }
    if (origin) builder.setOrigin(origin)
    if (!builder.origin) {
      builder.say('Tell me where to build: stand on the spot for the corner and say "build here", or set "origin" in config.json.')
      return
    }
    try {
      await builder.build()
    } catch (err) {
      log('Build error:', err.stack || err.message)
      builder.say(`Something went wrong: ${err.message}. Say "build" to try again.`)
    }
  }

  const handle = (text, fromPos) => {
    const cmd = text.trim().toLowerCase()
    if (cmd === 'build here') {
      if (!fromPos) return log('"build here" needs to be said in game (or set origin in config.json)')
      start(fromPos.floored())
    } else if (cmd === 'build' || cmd === 'start') start()
    else if (cmd === 'pause') { builder.pause(); builder.say('Paused. Say "resume" to carry on.') } else if (cmd === 'resume') { builder.resume(); builder.say('Resuming.') } else if (cmd === 'stop') { builder.stop(); builder.say('Stopping after this block.') } else if (cmd === 'status') builder.say(builder.status())
    else if (cmd === 'materials') {
      const missing = builder.missingMaterials()
      builder.say(Object.keys(missing).length ? 'Missing: ' + Object.entries(missing).map(([n, c]) => `${c} ${n}`).join(', ') : 'Nothing missing that I know of.')
    } else if (cmd === 'come') {
      const owner = cfg.owner && bot.players[cfg.owner]?.entity
      if (owner) {
        const { goals } = require('mineflayer-pathfinder')
        bot.pathfinder.goto(new goals.GoalNear(owner.position.x, owner.position.y, owner.position.z, 2)).catch(() => {})
      }
    } else if (cmd === 'quit') { bot.quit(); process.exit(0) } else return false
    return true
  }

  bot.once('spawn', async () => {
    log(`Joined as ${bot.username} (Minecraft ${bot.version}), game mode: ${bot.game.gameMode}`)
    const plan = await loadPlan(cfg, bot.registry)
    if (plan.unknown.length) log(`WARNING: not in ${bot.version}, skipping: ${plan.unknown.join(', ')}`)
    log(`Loaded ${path.basename(cfg.schematic)}: ${plan.steps.length} blocks`)
    builder = new Builder(bot, plan, cfg, log)
    const origin = cfg.origin ? new Vec3(cfg.origin[0], cfg.origin[1], cfg.origin[2]) : null
    if (origin) {
      await bot.waitForChunksToLoad()
      start(origin)
    } else {
      builder.say('Hi! Stand where the build\'s corner should go and say "build here".')
    }
  })

  const onChat = (username, message) => {
    if (!builder || username === bot.username) return
    if (cfg.owner && username !== cfg.owner) return
    const pos = bot.players[username]?.entity?.position
    handle(message, pos)
  }
  bot.on('chat', onChat)
  bot.on('whisper', onChat)

  bot.on('death', () => {
    log('Died! Pausing - items are wherever I died.')
    if (builder) builder.pause()
  })
  bot.on('kicked', reason => log('Kicked:', typeof reason === 'string' ? reason : JSON.stringify(reason)))
  bot.on('error', err => log('Error:', err.message))
  bot.on('end', reason => { log('Disconnected:', reason); process.exit(1) })

  // console commands too
  const rl = readline.createInterface({ input: process.stdin })
  rl.on('line', line => {
    if (!builder) return
    if (!handle(line, line.trim().toLowerCase() === 'build here' ? bot.entity.position : null)) {
      bot.chat(line)
    }
  })
}

async function main () {
  const [cmd = 'build', file = 'config.json'] = process.argv.slice(2)
  if (cmd === 'help' || cmd === '--help' || cmd === '-h') return console.log(USAGE)
  const cfg = loadConfig(file)
  if (cmd === 'plan') return cmdPlan(cfg)
  if (cmd === 'build') return cmdBuild(cfg)
  console.log(USAGE)
  process.exit(1)
}

main().catch(err => { console.error(err.message); process.exit(1) })
