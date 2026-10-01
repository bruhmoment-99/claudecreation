'use strict'
// Turns a chat line into a command object. Kept separate from the bot so it can be tested.

// "~", "~5", "~-3" are relative to whoever said it, like in Minecraft commands
function coord (token, base) {
  if (token === undefined) return NaN
  if (token.startsWith('~')) {
    if (base === undefined || base === null) return NaN
    const off = token.length > 1 ? Number(token.slice(1)) : 0
    return Math.floor(base) + off
  }
  return /^-?\d+$/.test(token) ? Number(token) : NaN
}

/**
 * @param {string} text
 * @param {{x:number,y:number,z:number}|null} speakerPos
 * @returns {object|null} null when it isn't a command
 */
function parseCommand (text, speakerPos) {
  const words = text.trim().split(/\s+/)
  const cmd = words[0]?.toLowerCase()
  const rest = words.slice(1)
  const lower = text.trim().toLowerCase()

  if (lower === 'build here') {
    if (!speakerPos) return { type: 'error', message: '"build here" has to be said in game. Use "build at <x> <y> <z>" instead.' }
    return { type: 'build', origin: floor(speakerPos) }
  }

  if (cmd === 'build' && rest[0]?.toLowerCase() === 'at') {
    const [xs, ys, zs, ...opts] = rest.slice(1)
    const p = speakerPos || {}
    const origin = { x: coord(xs, p.x), y: coord(ys, p.y), z: coord(zs, p.z) }
    if ([origin.x, origin.y, origin.z].some(Number.isNaN)) {
      return { type: 'error', message: 'Usage: build at <x> <y> <z> [rotate 90|180|270]  (~ works too, e.g. "build at ~10 ~ ~")' }
    }
    if (origin.y < -64 || origin.y > 319) return { type: 'error', message: `y=${origin.y} is outside the world (-64 to 319).` }
    const out = { type: 'build', origin }
    const ri = opts.findIndex(o => /^rotate(d)?$/i.test(o))
    if (ri >= 0) {
      const deg = Number(opts[ri + 1])
      if (![0, 90, 180, 270].includes(deg)) return { type: 'error', message: 'rotate must be 0, 90, 180 or 270.' }
      out.rotate = deg
    }
    return out
  }

  if (cmd === 'hunt' || cmd === 'kill') {
    if (!rest[0]) return { type: 'error', message: 'Usage: hunt <player>' }
    return { type: 'hunt', player: rest[0] }
  }

  const simple = {
    build: 'build',
    start: 'build',
    pause: 'pause',
    resume: 'resume',
    stop: 'stop',
    status: 'status',
    materials: 'materials',
    where: 'where',
    area: 'where',
    come: 'come',
    deliver: 'deliver',
    loot: 'deliver',
    guard: 'guard',
    protect: 'guard',
    follow: 'follow',
    help: 'help',
    quit: 'quit'
  }
  if (words.length === 1 && simple[cmd]) return { type: simple[cmd] }
  return null
}

function floor (p) { return { x: Math.floor(p.x), y: Math.floor(p.y), z: Math.floor(p.z) } }

const HELP = 'Commands: build here | build at <x> <y> <z> [rotate 90] | build | pause | resume | stop | status | materials | where | ' +
  'hunt <player> | deliver | guard | follow | come | quit'

module.exports = { parseCommand, HELP }
