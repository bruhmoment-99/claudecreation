'use strict'
// Combat jobs: hunt a named player and bring their drops back to the owner,
// guard the owner from mobs (and players who hit them), or just follow.

const { goals, Movements } = require('mineflayer-pathfinder')

const sleep = ms => new Promise(resolve => setTimeout(resolve, ms))

// Attack damage roughly in vanilla 1.21 terms, used to pick the best weapon
const WEAPON_DAMAGE = {
  netherite_sword: 8,
  diamond_sword: 7,
  iron_sword: 6,
  stone_sword: 5,
  golden_sword: 4,
  wooden_sword: 4,
  netherite_axe: 10,
  diamond_axe: 9,
  iron_axe: 9,
  stone_axe: 9,
  golden_axe: 7,
  wooden_axe: 7,
  mace: 6,
  trident: 9
}
// Ticks to wait between swings for a full-strength hit (20 / attack speed)
const COOLDOWN = { sword: 13, axe: 20, mace: 34, trident: 19, hand: 5 }
const ARMOR_RANK = ['leather', 'golden', 'chainmail', 'iron', 'diamond', 'netherite']
const ARMOR_SLOTS = { helmet: 'head', chestplate: 'torso', leggings: 'legs', boots: 'feet', turtle_helmet: 'head' }

class Fighter {
  constructor (bot, config, log, say) {
    this.bot = bot
    this.config = config
    this.log = log
    this.say = say
    this.job = null // { kind: 'hunt'|'guard'|'follow', ... }
    this.loot = {} // item name -> count picked up on hunts, waiting to be delivered
    this.lastSwing = new Map() // entity id -> time it last swung its arm

    bot.on('entitySwingArm', e => this.lastSwing.set(e.id, Date.now()))
    bot.on('entityHurt', e => this.onHurt(e))
    bot.on('entityDead', e => this.onDead(e))
    bot.on('messagestr', (msg, position) => this.onMessage(msg, position))
    bot.on('health', () => this.checkHealth())
    // our own tick clock: bot.time.age only updates when the server sends the time (~once a second)
    this.ticks = 0
    bot.on('physicsTick', () => { this.ticks++ })
  }

  get busy () { return this.job !== null }

  movements () {
    const m = new Movements(this.bot)
    m.canDig = false // don't tunnel through people's bases (or the cannon) while chasing
    m.allowParkour = true
    m.allowSprinting = true
    m.scafoldingBlocks = []
    return m
  }

  stop (quiet) {
    if (!this.job) return
    const kind = this.job.kind
    this.job.cancelled = true
    this.job = null
    this.bot.pathfinder.setGoal(null)
    if (!quiet) this.say(`Stopped ${kind === 'hunt' ? 'hunting' : kind === 'guard' ? 'guarding' : 'following'}.`)
  }

  // ------------------------------------------------------------------ gear

  async gearUp () {
    const bot = this.bot
    for (const item of bot.inventory.items()) {
      const m = /^(\w+?)_(helmet|chestplate|leggings|boots)$/.exec(item.name)
      const slot = item.name === 'turtle_helmet' ? 'head' : m && ARMOR_SLOTS[m[2]]
      if (!slot) continue
      const worn = bot.inventory.slots[bot.getEquipmentDestSlot(slot)]
      const rank = n => ARMOR_RANK.indexOf((/^(\w+?)_/.exec(n || '') || [])[1])
      if (!worn || rank(item.name) > rank(worn.name)) {
        try { await bot.equip(item, slot) } catch (_) {}
      }
    }
    return this.equipWeapon()
  }

  async equipWeapon () {
    const best = this.bot.inventory.items()
      .filter(i => WEAPON_DAMAGE[i.name])
      .sort((a, b) => dps(b.name) - dps(a.name))[0]
    if (!best) return null
    if (this.bot.heldItem?.name !== best.name) {
      try { await this.bot.equip(best, 'hand') } catch (_) {}
    }
    return best.name
  }

  cooldownTicks () {
    const held = this.bot.heldItem?.name || ''
    if (held.endsWith('_sword')) return COOLDOWN.sword
    if (held.endsWith('_axe')) return COOLDOWN.axe
    return COOLDOWN[held] || COOLDOWN.hand
  }

  // ------------------------------------------------------------------ fighting

  // Chase `getTarget()` and hit it until `done()` says stop. Resolves with why it ended.
  async fight (job, getTarget, done, { maxMs = 5 * 60 * 1000, leash = null } = {}) {
    const bot = this.bot
    bot.pathfinder.setMovements(this.movements())
    const startedAt = Date.now()
    let lastHit = -100
    let pathing = false
    try {
      while (!job.cancelled) {
        if (done()) return 'done'
        if (Date.now() - startedAt > maxMs) return 'timeout'
        if (bot.health <= (this.config.retreatHealth ?? 6)) return 'hurt'
        const target = getTarget()
        if (!target) return 'lost'
        if (leash && leash() > 24) return 'leash'

        const dist = bot.entity.position.distanceTo(target.position)
        const reach = reachTo(bot, target)
        const sameLevel = Math.abs(target.position.y - bot.entity.position.y) < 1.5

        if (dist > 6 || !sameLevel) {
          // far away or up/down a level: let the pathfinder work out the route
          if (!pathing) {
            bot.clearControlStates()
            bot.pathfinder.setGoal(new goals.GoalFollow(target, 1), true)
            pathing = true
          }
        } else {
          // close and on our level: steer straight at them like a player would
          if (pathing) { bot.pathfinder.setGoal(null); pathing = false }
          await bot.lookAt(target.position.offset(0, (target.height || 1.8) * 0.85, 0), true)
          bot.setControlState('forward', reach > 2.2)
          bot.setControlState('sprint', reach > 2.2)
          bot.setControlState('jump', reach > 2.2 && bot.entity.isCollidedHorizontally)
        }

        if (reach <= 2.9 && this.ticks - lastHit >= this.cooldownTicks()) {
          await bot.lookAt(target.position.offset(0, (target.height || 1.8) * 0.85, 0), true)
          bot.attack(target)
          lastHit = this.ticks
        }
        await bot.waitForTicks(1)
      }
      return 'cancelled'
    } finally {
      bot.clearControlStates()
      bot.pathfinder.setGoal(null)
    }
  }

  // ------------------------------------------------------------------ hunting

  async hunt (name) {
    const bot = this.bot
    const owner = this.config.owner
    if (!owner) return this.say('Set "owner" in config.json first - I only take hunting orders from my owner.')
    if (name.toLowerCase() === owner.toLowerCase()) return this.say('I\'m not hunting you.')
    if (name.toLowerCase() === bot.username.toLowerCase()) return this.say('Nice try.')
    const player = Object.keys(bot.players).find(p => p.toLowerCase() === name.toLowerCase())
    if (!player) return this.say(`${name} isn't online.`)

    this.stop(true)
    const job = { kind: 'hunt', target: player, killed: false, deathPos: null, lastPos: null }
    this.job = job
    const weapon = await this.gearUp()
    this.say(`Hunting ${player}${weapon ? ` with my ${weapon.replace(/_/g, ' ')}` : ' with my bare hands (give me a sword!)'}.`)

    const getTarget = () => {
      const e = bot.players[player]?.entity
      if (e) job.lastPos = e.position.clone()
      return e || null
    }

    // give them a moment to come into view
    for (let i = 0; i < 20 && !getTarget() && !job.cancelled; i++) await sleep(500)

    // If they're out of sight, head for where we last saw them, then wait a bit
    let result
    for (let tries = 0; tries < 6 && !job.cancelled; tries++) {
      result = await this.fight(job, getTarget, () => job.killed, { maxMs: (this.config.huntMinutes ?? 5) * 60 * 1000 })
      if (result !== 'lost' || job.killed) break
      if (!bot.players[player]) { result = 'left'; break }
      if (job.lastPos) {
        bot.pathfinder.setGoal(null)
        try { await bot.pathfinder.goto(new goals.GoalNear(job.lastPos.x, job.lastPos.y, job.lastPos.z, 2)) } catch (_) {}
      } else {
        this.say(`I can't see ${player} - they need to be within render distance of me.`)
        break
      }
      await sleep(1500)
    }
    if (job.cancelled) return

    bot.pathfinder.setGoal(null)
    if (job.killed || result === 'done') {
      this.say(`Got ${player}! Picking up their stuff...`)
      await this.collectLoot(job, job.deathPos || job.lastPos)
      this.job = null
      await this.deliver()
    } else {
      this.job = null
      const why = { hurt: 'I\'m too hurt to keep fighting', timeout: 'I couldn\'t catch them in time', left: `${player} left the game`, lost: `I lost track of ${player}` }[result] || result
      this.say(`Giving up the hunt: ${why}.`)
      if (result === 'hurt') await this.returnToOwner()
    }
  }

  onDead (entity) {
    const job = this.job
    if (job?.kind === 'hunt' && entity.type === 'player' && entity.username === job.target) {
      job.killed = true
      job.deathPos = entity.position.clone()
    }
  }

  // Backup for servers where the death event doesn't come through: "<name> was slain by <bot>"
  onMessage (msg, position) {
    const job = this.job
    if (job?.kind !== 'hunt' || position === 'chat') return
    if (msg.startsWith(job.target + ' ') && msg.includes(this.bot.username)) {
      job.killed = true
      if (!job.deathPos) job.deathPos = job.lastPos
    }
  }

  async collectLoot (job, where) {
    const bot = this.bot
    if (!where) return
    const before = countItems(bot)
    const until = Date.now() + 30000
    const unreachable = new Set()
    // drops appear right away in vanilla, but give laggy servers a few seconds
    for (let i = 0; i < 6 && !this.dropsNear(where, unreachable).length; i++) await sleep(500)
    while (Date.now() < until && !job.cancelled) {
      const drops = this.dropsNear(where, unreachable).sort((a, b) => a.position.distanceTo(bot.entity.position) - b.position.distanceTo(bot.entity.position))
      if (!drops.length) break
      if (bot.inventory.emptySlotCount() === 0) { this.say('My inventory is full, leaving the rest.'); break }
      const d = drops[0].position
      try {
        await withTimeout(bot.pathfinder.goto(new goals.GoalNear(d.x, d.y, d.z, 0.5)), 6000)
      } catch (_) {
        bot.pathfinder.setGoal(null)
        unreachable.add(drops[0].id) // in lava, down a hole... leave it
      }
      await bot.waitForTicks(3)
    }
    const after = countItems(bot)
    for (const [name, n] of Object.entries(after)) {
      const gained = n - (before[name] || 0)
      if (gained > 0) this.loot[name] = (this.loot[name] || 0) + gained
    }
  }

  dropsNear (where, skip) {
    return Object.values(this.bot.entities).filter(e => e.name === 'item' && !skip.has(e.id) && e.position.distanceTo(where) < 10)
  }

  async returnToOwner () {
    const owner = this.config.owner && this.bot.players[this.config.owner]?.entity
    if (!owner) return false
    this.bot.pathfinder.setMovements(this.movements())
    try {
      await this.bot.pathfinder.goto(new goals.GoalNear(owner.position.x, owner.position.y, owner.position.z, 2))
    } catch (_) {}
    return true
  }

  async deliver () {
    const bot = this.bot
    const total = Object.values(this.loot).reduce((a, b) => a + b, 0)
    if (!total) return this.say('No loot to hand over.')
    if (!await this.returnToOwner()) {
      return this.say(`I've got ${total} items of loot for you. Come near me and say "deliver".`)
    }
    const owner = bot.players[this.config.owner].entity
    await bot.lookAt(owner.position.offset(0, 1.6, 0), true)
    let handed = 0
    for (const [name, want] of Object.entries(this.loot)) {
      let left = want
      for (const item of bot.inventory.items().filter(i => i.name === name)) {
        if (left <= 0) break
        const n = Math.min(left, item.count)
        try { await bot.toss(item.type, null, n); left -= n; handed += n } catch (_) {}
      }
    }
    this.loot = {}
    this.say(`Here you go - ${handed} items.`)
  }

  // ------------------------------------------------------------------ guarding

  async guard () {
    const bot = this.bot
    const ownerName = this.config.owner
    if (!ownerName) return this.say('Set "owner" in config.json so I know who to guard.')
    this.stop(true)
    const job = { kind: 'guard' }
    this.job = job
    await this.gearUp()
    this.say('Guarding you. I\'ll fight mobs near you' + (this.config.guardAgainstPlayers !== false ? ' and anyone who hits you.' : '.'))
    bot.pathfinder.setMovements(this.movements())

    while (!job.cancelled) {
      const owner = bot.players[ownerName]?.entity
      if (!owner) { await sleep(1000); continue }
      const threat = job.attacker && bot.entities[job.attacker.id] ? job.attacker : this.nearestHostile(owner.position, 12)
      if (threat) {
        job.attacker = null
        await this.fight(job, () => bot.entities[threat.id] && threat.isValid !== false ? bot.entities[threat.id] : null,
          () => false, { maxMs: 60000, leash: () => threat.position.distanceTo(owner.position) })
        if (job.cancelled) break
        if (bot.health <= (this.config.retreatHealth ?? 6)) this.say('Ouch - I need food or healing.')
        bot.pathfinder.setMovements(this.movements())
      }
      bot.pathfinder.setGoal(new goals.GoalFollow(owner, 3), true)
      await bot.waitForTicks(5)
    }
  }

  nearestHostile (center, range) {
    let best = null
    for (const e of Object.values(this.bot.entities)) {
      if (e.type !== 'hostile' || !e.position) continue
      if (/^(enderman|zombified_piglin|piglin)$/.test(e.name)) continue // neutral until provoked
      const d = e.position.distanceTo(center)
      if (d < range && (!best || d < best.d)) best = { e, d }
    }
    return best?.e || null
  }

  // Owner got hurt: if a player right next to them just swung, that's who did it
  onHurt (entity) {
    const job = this.job
    if (job?.kind !== 'guard' || this.config.guardAgainstPlayers === false) return
    if (entity.type !== 'player' || entity.username !== this.config.owner) return
    const now = Date.now()
    for (const e of Object.values(this.bot.entities)) {
      if (e.type !== 'player' || e === entity || e === this.bot.entity) continue
      if (e.position.distanceTo(entity.position) < 5 && now - (this.lastSwing.get(e.id) || 0) < 800) {
        job.attacker = e
        this.say(`${e.username} is attacking you - on it.`)
        return
      }
    }
  }

  // ------------------------------------------------------------------ follow

  follow () {
    const owner = this.config.owner && this.bot.players[this.config.owner]?.entity
    if (!owner) return this.say('I can\'t see you.')
    this.stop(true)
    this.job = { kind: 'follow' }
    this.bot.pathfinder.setMovements(this.movements())
    this.bot.pathfinder.setGoal(new goals.GoalFollow(owner, 2), true)
    this.say('Following you.')
  }

  checkHealth () {
    if (this.job && this.bot.health <= 4 && !this.warnedLow) {
      this.warnedLow = true
      this.say(`I'm on ${Math.ceil(this.bot.health / 2)} hearts!`)
    }
    if (this.bot.health > 10) this.warnedLow = false
  }
}

// Distance from our eyes to the nearest point of the target's hitbox (what vanilla's 3-block reach measures)
function reachTo (bot, target) {
  const eye = bot.entity.position.offset(0, bot.entity.eyeHeight ?? 1.62, 0)
  const half = (target.width || 0.6) / 2
  const p = target.position
  const clamp = (v, lo, hi) => Math.max(lo, Math.min(hi, v))
  const dx = eye.x - clamp(eye.x, p.x - half, p.x + half)
  const dy = eye.y - clamp(eye.y, p.y, p.y + (target.height || 1.8))
  const dz = eye.z - clamp(eye.z, p.z - half, p.z + half)
  return Math.sqrt(dx * dx + dy * dy + dz * dz)
}

// damage per second, so a fast sword beats a slow axe
function dps (name) {
  const kind = name.endsWith('_sword') ? 'sword' : name.endsWith('_axe') ? 'axe' : name
  return WEAPON_DAMAGE[name] * 20 / (COOLDOWN[kind] || COOLDOWN.hand)
}

function countItems (bot) {
  const out = {}
  for (const i of bot.inventory.items()) out[i.name] = (out[i.name] || 0) + i.count
  return out
}

function withTimeout (promise, ms) {
  let timer
  return Promise.race([
    promise.finally(() => clearTimeout(timer)),
    new Promise((resolve, reject) => { timer = setTimeout(() => reject(new Error('timed out')), ms) })
  ])
}

module.exports = { Fighter, dps }
