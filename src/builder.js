'use strict'
// Drives a mineflayer bot through a build plan in survival: fetches materials from
// chests, clears the area, walks/pillars to each block, places it facing the right
// way, fixes repeater delays etc., and checks every block after placing it.

const { Vec3 } = require('vec3')
const { goals, Movements } = require('mineflayer-pathfinder')
const { DIRS, INTERACTIVE, lookAngles, itemNameFor } = require('./placement')
const { formatMaterials } = require('./planner')

const TOOL = /_(pickaxe|axe|shovel|hoe|sword)$|^shears$/
const REPLACEABLE = new Set(['air', 'cave_air', 'void_air', 'water', 'lava', 'short_grass', 'grass', 'tall_grass', 'fern', 'large_fern', 'dead_bush', 'snow', 'vine', 'seagrass', 'tall_seagrass', 'fire', 'bubble_column'])
const STORAGE = new Set(['chest', 'trapped_chest', 'barrel'])
const PRESSURE = /_pressure_plate$|^tripwire$/

const vec = d => new Vec3(d.x, d.y, d.z)
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms))

class Builder {
  constructor (bot, plan, config, log) {
    this.bot = bot
    this.plan = plan
    this.config = config
    this.log = log
    this.origin = null
    this.state = 'idle' // idle | building | paused | stopping | waiting | done
    this.chests = new Map() // "x,y,z" -> { pos, contents: { item: count } }
    this.placed = 0
    this.problems = new Map() // step -> reason
    this.queue = []
    this.reach = config.reach || 4.5
  }

  // ---------------------------------------------------------------- setup

  setOrigin (pos) {
    this.origin = pos.floored()
    const s = this.plan.size
    this.min = this.origin
    this.max = this.origin.offset(s.x - 1, s.y - 1, s.z - 1)
    this.expected = new Map()
    for (const step of this.plan.steps) this.expected.set(this.key(this.worldPos(step)), step)
    this.setupMovements()
    this.log(`Build area: ${fmt(this.min)} to ${fmt(this.max)} (${s.x} x ${s.y} x ${s.z})`)
  }

  key (p) { return `${p.x},${p.y},${p.z}` }
  worldPos (step) { return this.origin.offset(step.x, step.y, step.z) }
  inBox (p) {
    return p.x >= this.min.x && p.x <= this.max.x && p.y >= this.min.y && p.y <= this.max.y && p.z >= this.min.z && p.z <= this.max.z
  }

  setupMovements () {
    const bot = this.bot
    const reg = bot.registry
    const needed = new Set(Object.keys(this.plan.materials))
    const scaffold = (this.config.scaffoldBlocks || ['dirt', 'cobblestone', 'cobbled_deepslate', 'netherrack'])
      .filter(n => reg.itemsByName[n] && !needed.has(n)) // never burn build materials as scaffolding
    this.scaffoldNames = new Set(scaffold)

    const make = (allowBreakInBox) => {
      const m = new Movements(bot)
      m.allowParkour = false
      m.allowSprinting = true
      m.canOpenDoors = false
      m.scafoldingBlocks = scaffold.map(n => reg.itemsByName[n].id)
      if (!allowBreakInBox) {
        m.exclusionAreasBreak.push(block => this.inBox(block.position) ? Infinity : 0)
        // don't drop scaffolding into the build, it'd have to be dug out again
        m.exclusionAreasPlace.push(block => this.inBox(block.position) ? Infinity : 0)
        // don't trip pressure plates/tripwire that are already built
        m.exclusionAreasStep.push(block => this.inBox(block.position) && PRESSURE.test(block.name) ? Infinity : 0)
      }
      return m
    }
    this.buildMoves = make(false)
    this.clearMoves = make(true)
    bot.pathfinder.setMovements(this.buildMoves)
  }

  // ---------------------------------------------------------------- control

  async checkpoint () {
    while (this.state === 'paused') {
      this.parked = true
      await sleep(250)
    }
    this.parked = false
    if (this.state === 'stopping') throw new StopError()
    await this.eatIfHungry()
  }

  // Pause and wait until the build loop has actually let go of the bot (so something
  // else, like a hunt, can use the pathfinder). Returns true if a build was interrupted.
  async park () {
    if (this.state !== 'building' && this.state !== 'paused') return false
    this.pause()
    for (let i = 0; i < 120 && !this.parked && this.state === 'paused'; i++) await sleep(250)
    return true
  }

  // Walk to the site first if it's far away - chunks out of view aren't loaded
  async travelToSite () {
    const center = this.origin.offset(Math.floor(this.plan.size.x / 2), 0, Math.floor(this.plan.size.z / 2))
    const pos = this.bot.entity.position
    if (Math.hypot(pos.x - center.x, pos.z - center.z) < 48) return
    this.say(`Heading to the build site at ${fmt(this.origin)} (${Math.round(pos.distanceTo(center))} blocks away)...`)
    await this.goto(new goals.GoalNearXZ(center.x, center.z, 6))
    await this.bot.waitForChunksToLoad()
  }

  pause () { if (this.state === 'building') { this.state = 'paused'; this.bot.pathfinder.stop() } }
  resume () { if (this.state === 'paused') this.state = 'building' }
  stop () { if (this.state === 'building' || this.state === 'paused') { this.state = 'stopping'; this.bot.pathfinder.stop() } }

  status () {
    const total = this.plan.steps.length
    const left = this.queue.length
    return `${this.state}: ${this.placed} placed this session, ${left} of ${total} blocks still in the queue` +
      (this.problems.size ? `, ${this.problems.size} problem blocks` : '')
  }

  // ---------------------------------------------------------------- main loop

  async build () {
    if (!this.origin) throw new Error('No build origin set')
    if (this.state === 'building' || this.state === 'paused') return
    this.state = 'building'
    // forget what the chests held last time - someone (or a gatherer bot) may have restocked them
    this.chests.clear()
    try {
      await this.travelToSite()
      await this.surveyChests()
      this.reportMaterials()
      if (this.config.clearArea !== false) await this.clearArea()

      this.queue = this.plan.steps.slice()
      for (let pass = 1; this.queue.length && pass <= 4; pass++) {
        if (pass > 1) {
          const why = {}
          for (const st of this.queue) { const r = this.problems.get(st); why[r] = (why[r] || 0) + 1 }
          this.log(`Pass ${pass}: retrying ${this.queue.length} blocks (${Object.entries(why).map(([r, n]) => `${n} ${r}`).join(', ')})`)
        }
        const before = this.queue.length
        const retry = []
        for (let i = 0; i < this.queue.length; i++) {
          await this.checkpoint()
          const step = this.queue[i]
          this.cursor = i
          const result = await this.placeStep(step)
          if (result === 'ok') {
            this.problems.delete(step)
          } else {
            this.problems.set(step, result)
            retry.push(step)
          }
          if ((i + 1) % 50 === 0) this.log(`  ${i + 1}/${this.queue.length} this pass, ${retry.length} to retry`)
        }
        this.queue = retry
        if (retry.length === before) break // nothing changed, more passes won't help
      }

      await this.removeStrayScaffolding()
      this.finish()
    } catch (err) {
      if (err instanceof StopError) {
        this.log('Stopped. Say "build" to continue where it left off.')
        this.state = 'idle'
      } else {
        this.state = 'idle'
        throw err
      }
    } finally {
      this.bot.setControlState('sneak', false)
    }
  }

  finish () {
    if (this.problems.size === 0) {
      this.state = 'done'
      this.say('Build finished - every block placed and checked.')
      return
    }
    this.state = 'waiting'
    const byReason = {}
    for (const [step, reason] of this.problems) {
      const k = `${reason}: ${step.name}`
      byReason[k] = byReason[k] || []
      byReason[k].push(this.worldPos(step))
    }
    this.say(`Done what I can. ${this.problems.size} blocks still need attention (see console).`)
    for (const [k, positions] of Object.entries(byReason)) {
      this.log(`  ${positions.length} x ${k}  e.g. at ${positions.slice(0, 3).map(fmt).join('  ')}`)
    }
    if ([...this.problems.values()].includes('missing item')) {
      this.say('Put the missing items in a chest near the build and say "build" to carry on.')
      this.log(formatMaterials(this.missingMaterials(), null))
    }
  }

  missingMaterials () {
    const need = {}
    for (const [step, reason] of this.problems) {
      if (reason === 'missing item') need[step.how.item] = (need[step.how.item] || 0) + 1
    }
    return need
  }

  // ---------------------------------------------------------------- placing

  isDone (step) {
    const block = this.bot.blockAt(this.worldPos(step))
    return !!block && this.matches(block, step) && this.clicksNeeded(block, step) === 0
  }

  matches (block, step) {
    if (block.name !== step.name) return false
    const props = block.getProperties()
    if (step.how.bucket) return String(props.level) === '0'
    return step.how.check.every(k => !(k in step.props) || clickable(k) || String(props[k]) === String(step.props[k]))
  }

  // How many right-clicks turn what's there into what we want (repeater delay, note pitch...)
  clicksNeeded (block, step) {
    const now = block.getProperties()
    const want = step.props
    let n = 0
    if ('delay' in want && step.name === 'repeater') n += (Number(want.delay) - Number(now.delay) + 4) % 4
    if ('note' in want && step.name === 'note_block') n += (Number(want.note) - Number(now.note) + 25) % 25
    if ('mode' in want && step.name === 'comparator' && want.mode !== now.mode) n += 1
    if ('inverted' in want && step.name === 'daylight_detector' && String(want.inverted) !== String(now.inverted)) n += 1
    if ('open' in want && !/^iron_/.test(step.name) && /_(door|trapdoor|fence_gate)$/.test(step.name) && String(want.open) !== String(now.open)) n += 1
    return n
  }

  async placeStep (step) {
    const pos = this.worldPos(step)
    const how = step.how

    for (let attempt = 0; attempt < how.attempts.length * 2; attempt++) {
      let block = this.bot.blockAt(pos)
      if (block && this.matches(block, step)) {
        if (this.clicksNeeded(block, step) === 0 && !(how.doubleSlab && block.getProperties().type !== 'double')) return 'ok'
        if (how.doubleSlab && block.getProperties().type !== 'double') {
          if (!await this.topUpSlab(step, pos)) return 'could not place'
          continue
        }
        await this.adjustClicks(step, pos)
        continue
      }

      // something else is in the way (wrong block, or the right block facing the wrong way)
      if (block && !REPLACEABLE.has(block.name)) {
        const cleared = await this.clearBlock(pos)
        if (!cleared) return 'could not clear the spot'
        block = this.bot.blockAt(pos)
      }

      if (how.bucket) return await this.placeFluid(step, pos)

      // use this attempt's way of placing if possible, otherwise any way that has something to click on
      const rotated = how.attempts.map((_, i) => how.attempts[(attempt + i) % how.attempts.length])
      const option = rotated.find(o => this.referenceDirs(pos, o.refDirs).length > 0)
      if (!option) return 'nothing to place against yet'
      const refs = this.referenceDirs(pos, option.refDirs)
      if (!await this.getItem(how.item)) return 'missing item'

      try {
        await this.goto(new goals.GoalPlaceBlock(pos, this.bot.world, {
          range: this.reach - 0.3,
          LOS: !option.look,
          faces: refs.map(r => vec(DIRS[r])),
          half: option.half
        }))
      } catch (err) {
        if (err instanceof StopError) throw err
        return 'could not get there'
      }

      const ref = this.pickReference(pos, refs)
      if (!ref) continue
      if (!await this.equip(how.item)) return 'missing item'

      const placedOk = await this.clickPlace(ref.block, ref.face, option)
      if (!placedOk) continue
      if (how.doubleSlab) await this.topUpSlab(step, pos)
      await this.adjustClicks(step, pos)

      block = this.bot.blockAt(pos)
      if (block && this.matches(block, step) && this.clicksNeeded(block, step) === 0) {
        this.placed++
        return 'ok'
      }
      if (block && block.name === step.name) {
        this.log(`  ${step.name} at ${fmt(pos)} came out ${JSON.stringify(pick(block.getProperties(), how.check))}, wanted ${JSON.stringify(pick(step.props, how.check))} - redoing`)
      }
    }
    return 'came out wrong after several tries'
  }

  // Neighbours we can click on to put a block at pos
  referenceDirs (pos, dirNames) {
    const ok = []
    const interactive = []
    for (const d of dirNames) {
      const b = this.bot.blockAt(pos.plus(vec(DIRS[d])))
      if (!b || !isSolidish(b)) continue
      if (INTERACTIVE.test(b.name)) interactive.push(d)
      else ok.push(d)
    }
    return ok.concat(interactive) // we sneak while placing, but prefer not to click on levers & co. at all
  }

  pickReference (pos, dirNames) {
    const eye = this.bot.entity.position.offset(0, this.bot.entity.eyeHeight ?? 1.62, 0)
    for (const d of dirNames) {
      const dir = vec(DIRS[d])
      const refPos = pos.plus(dir)
      const block = this.bot.blockAt(refPos)
      if (!block || !isSolidish(block)) continue
      const faceCenter = pos.offset(0.5, 0.5, 0.5).plus(dir.scaled(0.5))
      if (faceCenter.distanceTo(eye) <= this.reach) return { block, face: dir.scaled(-1) }
    }
    return null
  }

  async clickPlace (refBlock, face, option) {
    const bot = this.bot
    const angles = lookAngles(option.look)
    try {
      if (angles) {
        await bot.look(angles.yaw, angles.pitch, true)
        await bot.waitForTicks(2) // make sure the server has our rotation before we click
      }
      bot.setControlState('sneak', true) // so clicking a chest/repeater doesn't open/toggle it
      await bot.waitForTicks(1)
      await bot._placeBlockWithOptions(refBlock, face, {
        forceLook: angles ? 'ignore' : true,
        half: option.half,
        swingArm: 'right'
      })
      return true
    } catch (err) {
      return false
    } finally {
      bot.setControlState('sneak', false)
      await bot.waitForTicks(2)
    }
  }

  async topUpSlab (step, pos) {
    const block = this.bot.blockAt(pos)
    if (!block || block.name !== step.name || block.getProperties().type === 'double') return true
    if (!await this.equip(step.how.item)) return false
    const top = block.getProperties().type === 'bottom'
    const face = new Vec3(0, top ? 1 : -1, 0)
    try {
      await this.bot._placeBlockWithOptions(block, face, { forceLook: true, delta: new Vec3(0.5, 0.5, 0.5), swingArm: 'right' })
    } catch (_) { return false }
    await this.bot.waitForTicks(2)
    return true
  }

  async adjustClicks (step, pos) {
    for (let guard = 0; guard < 30; guard++) {
      const block = this.bot.blockAt(pos)
      if (!block || block.name !== step.name || this.clicksNeeded(block, step) === 0) return
      try {
        await this.goto(new goals.GoalLookAtBlock(pos, this.bot.world, { reach: this.reach - 0.3 }))
        this.bot.setControlState('sneak', false)
        // empty hand, so the click can only toggle the block and never places another one
        if (this.bot.heldItem) await this.bot.unequip('hand').catch(() => {})
        await this.bot.activateBlock(block)
      } catch (err) {
        if (err instanceof StopError) throw err
        return
      }
      await this.bot.waitForTicks(3)
    }
  }

  // ---------------------------------------------------------------- fluids

  async placeFluid (step, pos) {
    const fluid = step.name
    const full = `${fluid}_bucket`
    if (!this.count(full)) {
      if (!this.count('bucket') && !await this.getItem('bucket')) return 'missing item'
      if (!await this.refillBucket(fluid)) return `no ${fluid} source to refill from`
    }
    const refs = this.referenceDirs(pos, ['down', 'north', 'south', 'west', 'east', 'up'])
    if (refs.length === 0) return 'nothing to place against yet'
    try {
      await this.goto(new goals.GoalPlaceBlock(pos, this.bot.world, { range: this.reach - 0.3, LOS: true, faces: refs.map(r => vec(DIRS[r])) }))
    } catch (err) {
      if (err instanceof StopError) throw err
      return 'could not get there'
    }
    const ref = this.pickReference(pos, refs)
    if (!ref || !await this.equip(full)) return 'could not place'
    const target = ref.block.position.offset(0.5, 0.5, 0.5).plus(ref.face.scaled(0.5))
    await this.bot.lookAt(target, true)
    await this.bot.waitForTicks(1)
    this.bot.activateItem()
    await this.bot.waitForTicks(4)
    const block = this.bot.blockAt(pos)
    return block && this.matches(block, step) ? (this.placed++, 'ok') : 'could not place'
  }

  async refillBucket (fluid) {
    const bot = this.bot
    const id = bot.registry.blocksByName[fluid].id
    const source = bot.findBlock({
      matching: b => b.type === id && String(b.getProperties().level) === '0' && !this.inBox(b.position),
      maxDistance: 64
    })
    if (!source) return false
    try {
      await this.goto(new goals.GoalNear(source.position.x, source.position.y + 1, source.position.z, 2))
    } catch (err) {
      if (err instanceof StopError) throw err
      return false
    }
    if (!await this.equip('bucket')) return false
    await bot.lookAt(source.position.offset(0.5, 0.9, 0.5), true)
    await bot.waitForTicks(1)
    bot.activateItem()
    await bot.waitForTicks(5)
    return this.count(`${fluid}_bucket`) > 0
  }

  // ---------------------------------------------------------------- clearing

  async clearArea () {
    const bot = this.bot
    const todo = []
    for (let y = this.max.y; y >= this.min.y; y--) {
      for (let x = this.min.x; x <= this.max.x; x++) {
        for (let z = this.min.z; z <= this.max.z; z++) {
          const p = new Vec3(x, y, z)
          if (this.expected.has(this.key(p))) continue // handled when placing that block
          const b = bot.blockAt(p)
          if (b && !REPLACEABLE.has(b.name)) todo.push(p)
        }
      }
    }
    if (!todo.length) return
    this.log(`Clearing ${todo.length} blocks out of the build area first`)
    bot.pathfinder.setMovements(this.clearMoves)
    try {
      for (const p of todo) {
        await this.checkpoint()
        await this.clearBlock(p)
      }
    } finally {
      bot.pathfinder.setMovements(this.buildMoves)
    }
  }

  async clearBlock (pos) {
    const bot = this.bot
    let block = bot.blockAt(pos)
    if (!block || REPLACEABLE.has(block.name)) return true
    if (block.hardness === null || block.hardness < 0 || block.name === 'bedrock') {
      this.log(`  can't break ${block.name} at ${fmt(pos)}`)
      return false
    }
    try {
      await this.goto(new goals.GoalLookAtBlock(pos, bot.world, { reach: this.reach - 0.3 }))
      block = bot.blockAt(pos)
      if (!block || REPLACEABLE.has(block.name)) return true
      const tool = bot.pathfinder.bestHarvestTool(block)
      if (tool) await bot.equip(tool, 'hand')
      else if (!block.canHarvest(bot.heldItem ? bot.heldItem.type : null)) await bot.unequip('hand').catch(() => {})
      const dropped = block.name
      await bot.dig(block, true)
      await bot.waitForTicks(2)
      // a wrongly placed build block: go and pick it up again, or we'll run out
      if (this.plan.materials[itemNameFor(dropped)]) await this.collectDrops(pos)
    } catch (err) {
      if (err instanceof StopError) throw err
      return false
    }
    const after = bot.blockAt(pos)
    return !after || REPLACEABLE.has(after.name)
  }

  async collectDrops (pos) {
    const bot = this.bot
    await bot.waitForTicks(10) // item entities need a moment to appear (and can't be picked up instantly)
    const center = pos.offset(0.5, 0.5, 0.5)
    const drops = Object.values(bot.entities).filter(e => e.name === 'item' && e.position.distanceTo(center) < 3)
    for (const drop of drops) {
      if (!bot.entities[drop.id]) continue
      try {
        const p = drop.position
        await withTimeout(this.goto(new goals.GoalNear(p.x, p.y, p.z, 1)), 8000)
        await bot.waitForTicks(4)
      } catch (err) {
        if (err instanceof StopError) throw err
        bot.pathfinder.stop()
      }
    }
  }

  async removeStrayScaffolding () {
    const strays = []
    for (let y = this.max.y; y >= this.min.y; y--) {
      for (let x = this.min.x; x <= this.max.x; x++) {
        for (let z = this.min.z; z <= this.max.z; z++) {
          const p = new Vec3(x, y, z)
          if (this.expected.has(this.key(p))) continue
          const b = this.bot.blockAt(p)
          if (b && this.scaffoldNames.has(b.name)) strays.push(p)
        }
      }
    }
    if (!strays.length) return
    this.log(`Removing ${strays.length} scaffolding blocks left inside the build`)
    for (const p of strays) {
      await this.checkpoint()
      await this.clearBlock(p)
    }
  }

  // ---------------------------------------------------------------- inventory & chests

  count (name) {
    return this.bot.inventory.items().filter(i => i.name === name).reduce((n, i) => n + i.count, 0)
  }

  async equip (name) {
    const item = this.bot.inventory.items().find(i => i.name === name)
    if (!item) return false
    try {
      await this.bot.equip(item, 'hand')
      return this.bot.heldItem?.name === name
    } catch (_) { return false }
  }

  async getItem (name) {
    if (this.count(name) > 0) return true
    await this.restock(name)
    return this.count(name) > 0
  }

  findChests () {
    const bot = this.bot
    const ids = [...STORAGE].map(n => bot.registry.blocksByName[n]?.id).filter(id => id !== undefined)
    const center = this.origin.offset(Math.floor(this.plan.size.x / 2), 0, Math.floor(this.plan.size.z / 2))
    const radius = this.config.chestSearchRadius ?? 32
    const found = new Map()
    for (const c of this.config.chests || []) {
      const p = new Vec3(c[0], c[1], c[2])
      found.set(this.key(p), p)
    }
    for (const p of bot.findBlocks({ matching: ids, maxDistance: radius + Math.max(this.plan.size.x, this.plan.size.z), count: 200, point: center })) {
      if (this.inBox(p)) continue // part of the build, hands off
      const b = bot.blockAt(p)
      if (b && b.getProperties().type === 'right') continue // other half of a double chest
      found.set(this.key(p), p)
    }
    for (const [k, p] of found) {
      if (!this.chests.has(k)) this.chests.set(k, { pos: p, contents: null })
    }
  }

  async surveyChests () {
    this.findChests()
    const unknown = [...this.chests.values()].filter(c => c.contents === null)
    if (!unknown.length) return
    this.log(`Checking ${unknown.length} chest(s) near the build for materials...`)
    for (const chest of unknown) {
      await this.checkpoint()
      await this.withChest(chest, () => {})
    }
  }

  available () {
    const have = {}
    for (const i of this.bot.inventory.items()) have[i.name] = (have[i.name] || 0) + i.count
    for (const c of this.chests.values()) {
      for (const [name, n] of Object.entries(c.contents || {})) have[name] = (have[name] || 0) + n
    }
    return have
  }

  reportMaterials () {
    const have = this.available()
    // count what's still to be placed, not what's already standing
    const need = {}
    for (const s of this.plan.steps) {
      if (s.how.bucket || this.isDone(s)) continue
      need[s.how.item] = (need[s.how.item] || 0) + (s.how.doubleSlab ? 2 : 1)
    }
    for (const fluid of Object.keys(this.plan.fluidSources || {})) {
      if (!have[`${fluid}_bucket`]) need.bucket = Math.max(need.bucket || 0, 1)
    }
    const short = Object.entries(need).filter(([n, c]) => (have[n] || 0) < c)
    this.log('Materials still needed (inventory + nearby chests):\n' + formatMaterials(need, have))
    if (short.length) {
      this.say(`Short on ${short.length} material(s): ${short.slice(0, 6).map(([n, c]) => `${c - (have[n] || 0)} ${n}`).join(', ')}${short.length > 6 ? '...' : ''}. I'll build what I can.`)
    } else {
      this.say('All materials found. Building now.')
    }
  }

  // What the rest of the queue still needs, in the order it'll need it
  upcomingNeeds () {
    const needs = new Map()
    const from = this.cursor || 0
    const steps = this.queue.length ? this.queue.slice(from) : this.plan.steps
    for (const s of steps) {
      if (s.how.bucket) continue
      needs.set(s.how.item, (needs.get(s.how.item) || 0) + (s.how.doubleSlab ? 2 : 1))
    }
    return needs
  }

  async restock (urgent) {
    this.findChests()
    const needs = this.upcomingNeeds()
    if (urgent && !needs.has(urgent)) needs.set(urgent, 1)
    // urgent item first
    const order = [urgent, ...[...needs.keys()].filter(n => n !== urgent)].filter(Boolean)

    const candidates = [...this.chests.values()]
      .filter(c => c.contents === null || (c.contents[urgent] || 0) > 0)
      .sort((a, b) => a.pos.distanceTo(this.bot.entity.position) - b.pos.distanceTo(this.bot.entity.position))

    for (const chest of candidates) {
      await this.withChest(chest, async (window) => {
        await this.depositJunk(window, needs)
        for (const name of order) {
          const want = (needs.get(name) || 0) - this.count(name)
          if (want <= 0) continue
          const item = this.bot.registry.itemsByName[name]
          if (!item) continue
          const inChest = window.containerItems().filter(i => i.name === name).reduce((n, i) => n + i.count, 0)
          const room = this.roomFor(name)
          const take = Math.min(want, inChest, room)
          if (take <= 0) continue
          try {
            await window.withdraw(item.id, null, take)
          } catch (err) {
            this.log(`  couldn't take ${name} from chest: ${err.message}`)
          }
        }
      })
      if (this.count(urgent) > 0) return
    }
  }

  roomFor (name) {
    const item = this.bot.registry.itemsByName[name]
    const stack = item ? item.stackSize : 64
    const partial = this.bot.inventory.items().filter(i => i.name === name).reduce((n, i) => n + (stack - i.count), 0)
    const free = Math.max(0, this.bot.inventory.emptySlotCount() - 2) // keep a couple free for pickups
    return partial + free * stack
  }

  async depositJunk (window, needs) {
    const keep = new Set([...needs.keys(), 'bucket', 'water_bucket', 'lava_bucket', ...this.scaffoldNames])
    const foods = this.bot.registry.foodsByName || {}
    for (const item of this.bot.inventory.items()) {
      if (keep.has(item.name) || TOOL.test(item.name) || foods[item.name]) continue
      if (this.scaffoldNames.has(item.name)) continue
      try { await window.deposit(item.type, null, item.count) } catch (_) { return } // chest full
    }
  }

  async withChest (chest, fn) {
    const bot = this.bot
    try {
      await this.goto(new goals.GoalLookAtBlock(chest.pos, bot.world, { reach: this.reach - 0.3 }))
      const block = bot.blockAt(chest.pos)
      if (!block || !STORAGE.has(block.name)) { this.chests.delete(this.key(chest.pos)); return }
      bot.setControlState('sneak', false)
      const window = await bot.openContainer(block)
      try {
        await fn(window)
        chest.contents = {}
        for (const i of window.containerItems()) chest.contents[i.name] = (chest.contents[i.name] || 0) + i.count
      } finally {
        window.close()
      }
    } catch (err) {
      if (err instanceof StopError) throw err
      this.log(`  couldn't use chest at ${fmt(chest.pos)}: ${err.message}`)
      if (chest.contents === null) chest.contents = {}
    }
  }

  // ---------------------------------------------------------------- survival bits

  async eatIfHungry () {
    const bot = this.bot
    if (bot.food >= 15 || this.eating) return
    const foods = bot.registry.foodsByName || {}
    const food = bot.inventory.items()
      .filter(i => foods[i.name] && !/^(rotten_flesh|spider_eye|poisonous_potato|pufferfish|suspicious_stew|chorus_fruit)$/.test(i.name))
      .sort((a, b) => foods[b.name].foodPoints - foods[a.name].foodPoints)[0]
    if (!food) {
      if (!this.warnedFood) { this.say('I\'m getting hungry and have no food - put some in a chest near the build.'); this.warnedFood = true }
      return
    }
    this.eating = true
    try {
      await bot.equip(food, 'hand')
      await bot.consume()
    } catch (_) {
    } finally {
      this.eating = false
    }
  }

  async goto (goal) {
    if (this.state === 'stopping') throw new StopError()
    try {
      await this.bot.pathfinder.goto(goal)
    } catch (err) {
      if (this.state === 'stopping') throw new StopError()
      if (this.state === 'paused') { await this.checkpoint(); return this.goto(goal) }
      throw err
    }
  }

  say (msg) {
    this.log(msg)
    const owner = this.config.owner
    try {
      if (owner && this.bot.players[owner]) this.bot.whisper(owner, msg)
      else if (this.config.chat !== false) this.bot.chat(msg)
    } catch (_) {}
  }
}

class StopError extends Error {}

function withTimeout (promise, ms) {
  let timer
  return Promise.race([
    promise.finally(() => clearTimeout(timer)),
    new Promise((resolve, reject) => { timer = setTimeout(() => reject(new Error('timed out')), ms) })
  ])
}

function isSolidish (block) {
  return block.boundingBox === 'block' && !REPLACEABLE.has(block.name)
}

function clickable (k) { return k === 'delay' || k === 'note' || k === 'mode' || k === 'inverted' || k === 'open' }

function pick (obj, keys) {
  const out = {}
  for (const k of keys) if (k in obj) out[k] = obj[k]
  return out
}

function fmt (p) { return `${p.x} ${p.y} ${p.z}` }

module.exports = { Builder, StopError }
