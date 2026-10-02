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
  node src/index.js mindcraft [config.json]  set up (and start) a Mindcraft bot that gathers the materials

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
  const { Fighter } = require('./combat')
  const { parseCommand, HELP } = require('./commands')

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
  let fighter = null
  let resumeBuildAfterJob = false
  let retryTimer = null
  let rotate = cfg.rotate || 0

  const start = async (origin, newRotate) => {
    if (builder.state === 'building' || builder.state === 'paused') {
      return builder.say(`Already ${builder.state}. Say "stop" first if you want to move the build.`)
    }
    if (fighter.busy) fighter.stop(true)
    resumeBuildAfterJob = false
    if (newRotate !== undefined && newRotate !== rotate) {
      rotate = newRotate
      builder.plan = await loadPlan({ ...cfg, rotate }, bot.registry)
    }
    if (origin) builder.setOrigin(new Vec3(origin.x, origin.y, origin.z))
    if (!builder.origin) {
      builder.say('Tell me where to build: say "build at <x> <y> <z>", or stand on the corner and say "build here".')
      return
    }
    builder.bot.pathfinder.setMovements(builder.buildMoves)
    try {
      await builder.build()
    } catch (err) {
      log('Build error:', err.stack || err.message)
      builder.say(`Something went wrong: ${err.message}. Say "build" to try again.`)
    }
    // Short on materials? Check the chests again later - a gatherer bot may be filling them
    const retry = cfg.retryMinutes ?? 0
    if (builder.state === 'waiting' && retry > 0) {
      log(`Checking the chests again in ${retry} minute(s)...`)
      clearTimeout(retryTimer)
      retryTimer = setTimeout(() => { if (builder.state === 'waiting') start() }, retry * 60 * 1000)
    }
  }

  // Hunting/guarding/following borrow the bot from the build, which picks up again afterwards
  const runJob = async (job) => {
    if (await builder.park()) {
      resumeBuildAfterJob = true
      builder.say('Pausing the build for this.')
    }
    try {
      await job()
    } catch (err) {
      log('Job error:', err.stack || err.message)
    }
    if (!fighter.busy) maybeResumeBuild()
  }

  const maybeResumeBuild = () => {
    if (!resumeBuildAfterJob) return
    resumeBuildAfterJob = false
    builder.bot.pathfinder.setMovements(builder.buildMoves)
    builder.resume()
    builder.say('Back to building.')
  }

  const handle = (text, fromPos) => {
    const c = parseCommand(text, fromPos)
    if (!c) return false
    switch (c.type) {
      case 'error': builder.say(c.message); break
      case 'help': builder.say(HELP); break
      case 'build': start(c.origin, c.rotate); break
      case 'pause': builder.pause(); builder.say('Paused. Say "resume" to carry on.'); break
      case 'resume': builder.bot.pathfinder.setMovements(builder.buildMoves); builder.resume(); builder.say('Resuming.'); break
      case 'stop':
        // first "stop" ends a hunt/guard/follow, the next one stops the build
        if (fighter.busy) { fighter.stop(); maybeResumeBuild() } else { builder.stop(); builder.say('Stopping after this block.') }
        break
      case 'status': builder.say(builder.status() + (fighter.busy ? ` (currently: ${fighter.job.kind}${fighter.job.target ? ' ' + fighter.job.target : ''})` : '')); break
      case 'where':
        builder.say(builder.origin
          ? `Build goes from ${builder.min.x} ${builder.min.y} ${builder.min.z} to ${builder.max.x} ${builder.max.y} ${builder.max.z}${rotate ? ` (rotated ${rotate})` : ''}.`
          : `No spot picked yet. The build is ${builder.plan.size.x} x ${builder.plan.size.y} x ${builder.plan.size.z} (x, y, z).`)
        break
      case 'materials': {
        const missing = builder.missingMaterials()
        builder.say(Object.keys(missing).length ? 'Missing: ' + Object.entries(missing).map(([n, k]) => `${k} ${n}`).join(', ') : 'Nothing missing that I know of.')
        break
      }
      case 'hunt': runJob(() => fighter.hunt(c.player)); break
      case 'guard': runJob(() => fighter.guard()); break
      case 'follow': runJob(async () => fighter.follow()); break
      case 'deliver': runJob(() => fighter.deliver()); break
      case 'come': runJob(() => fighter.returnToOwner()); break
      case 'quit': bot.quit(); process.exit(0)
    }
    return true
  }

  bot.once('spawn', async () => {
    log(`Joined as ${bot.username} (Minecraft ${bot.version}), game mode: ${bot.game.gameMode}`)
    const plan = await loadPlan(cfg, bot.registry)
    if (plan.unknown.length) log(`WARNING: not in ${bot.version}, skipping: ${plan.unknown.join(', ')}`)
    log(`Loaded ${path.basename(cfg.schematic)}: ${plan.steps.length} blocks`)
    builder = new Builder(bot, plan, cfg, log)
    fighter = new Fighter(bot, cfg, log, msg => builder.say(msg))
    const origin = cfg.origin ? { x: cfg.origin[0], y: cfg.origin[1], z: cfg.origin[2] } : null
    if (origin) {
      await bot.waitForChunksToLoad()
      start(origin)
    } else {
      builder.say('Hi! Say "build at <x> <y> <z>" (or stand on the spot and say "build here"). Say "help" for everything else.')
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
    if (fighter) fighter.stop(true)
    if (builder) builder.pause()
  })
  bot.on('kicked', reason => log('Kicked:', typeof reason === 'string' ? reason : JSON.stringify(reason)))
  bot.on('error', err => log('Error:', err.message))
  bot.on('end', reason => { log('Disconnected:', reason); process.exit(1) })

  // console commands too
  const rl = readline.createInterface({ input: process.stdin })
  rl.on('line', line => {
    if (!builder) return
    // in the terminal, "here" and "~" mean the bot's own position
    if (!handle(line, bot.entity.position)) bot.chat(line)
  })
}

async function main () {
  const [cmd = 'build', file = 'config.json'] = process.argv.slice(2)
  if (cmd === 'help' || cmd === '--help' || cmd === '-h') return console.log(USAGE)
  const cfg = loadConfig(file)
  if (cmd === 'plan') return cmdPlan(cfg)
  if (cmd === 'build') return cmdBuild(cfg)
  if (cmd === 'mindcraft') return require('./mindcraft').run(cfg, await loadPlan(cfg, require('minecraft-data')(cfg.version || '1.21.4')))
  console.log(USAGE)
  process.exit(1)
}

main().catch(err => { console.error(err.message); process.exit(1) })
