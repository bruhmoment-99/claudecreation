'use strict'
// Works out *how* to place each block state in vanilla Java Edition survival:
// which item to hold, which neighbouring block to click, which way the player
// must be looking (that's what decides facing for repeaters, pistons, observers...),
// how many right-clicks to do afterwards (repeater delay, comparator mode, note pitch)
// and which properties to check once it's down.

const DIRS = {
  north: { x: 0, y: 0, z: -1 },
  south: { x: 0, y: 0, z: 1 },
  west: { x: -1, y: 0, z: 0 },
  east: { x: 1, y: 0, z: 0 },
  up: { x: 0, y: 1, z: 0 },
  down: { x: 0, y: -1, z: 0 }
}
const OPPOSITE = { north: 'south', south: 'north', west: 'east', east: 'west', up: 'down', down: 'up' }
const HORIZONTAL = ['north', 'south', 'west', 'east']
const ALL = ['down', 'north', 'south', 'west', 'east', 'up']
const AXIS_DIRS = { x: ['west', 'east'], y: ['down', 'up'], z: ['north', 'south'] }

// Blocks that open a GUI or change state when right-clicked. The bot sneaks while
// placing anyway, but it also avoids clicking these as a reference when it can.
const INTERACTIVE = /(^|_)(repeater|comparator|note_block|lever|button|door|trapdoor|fence_gate|chest|barrel|furnace|smoker|hopper|dispenser|dropper|crafting_table|crafter|anvil|daylight_detector|bed|shulker_box|lectern|loom|grindstone|stonecutter|smithing_table|cartography_table|enchanting_table|brewing_stand|beacon|cake|flower_pot|jukebox|bell|respawn_anchor|composter|cauldron|campfire|sign|command_block|structure_block)$/

const SKIP = new Set([
  'piston_head', 'moving_piston', 'fire', 'soul_fire', 'nether_portal', 'end_portal', 'end_gateway',
  'bubble_column', 'tripwire', 'redstone_wire_placeholder'
])

// Blocks whose item has a different name
const ITEM_OVERRIDES = {
  redstone_wire: 'redstone',
  wall_torch: 'torch',
  soul_wall_torch: 'soul_torch',
  redstone_wall_torch: 'redstone_torch',
  copper_wall_torch: 'copper_torch',
  water: 'water_bucket',
  lava: 'lava_bucket',
  tripwire: 'string',
  powder_snow: 'powder_snow_bucket',
  kelp_plant: 'kelp',
  cave_vines_plant: 'glow_berries',
  cave_vines: 'glow_berries',
  sweet_berry_bush: 'sweet_berries',
  bamboo_sapling: 'bamboo',
  pitcher_crop: 'pitcher_pod',
  carrots: 'carrot',
  potatoes: 'potato',
  beetroots: 'beetroot_seeds',
  wheat: 'wheat_seeds',
  cocoa: 'cocoa_beans',
  tall_seagrass: 'seagrass',
  twisting_vines_plant: 'twisting_vines',
  weeping_vines_plant: 'weeping_vines',
  big_dripleaf_stem: 'big_dripleaf'
}

function itemNameFor (blockName) {
  if (ITEM_OVERRIDES[blockName]) return ITEM_OVERRIDES[blockName]
  if (blockName.endsWith('_wall_sign')) return blockName.replace('_wall_sign', '_sign')
  if (blockName.endsWith('_wall_hanging_sign')) return blockName.replace('_wall_hanging_sign', '_hanging_sign')
  if (blockName.endsWith('_wall_banner')) return blockName.replace('_wall_banner', '_banner')
  if (blockName.endsWith('_wall_head')) return blockName.replace('_wall_head', '_head')
  if (blockName.endsWith('_wall_skull')) return blockName.replace('_wall_skull', '_skull')
  if (blockName.endsWith('_wall_fan')) return blockName.replace('_wall_fan', '_fan')
  if (blockName.startsWith('potted_')) return 'flower_pot'
  return blockName
}

const is = (name, ...patterns) => patterns.some(p => p instanceof RegExp ? p.test(name) : p === name)

function category (name) {
  if (name === 'tnt') return 'tnt'
  if (name === 'water' || name === 'lava') return 'fluid'
  // Things that fire, move or power stuff the moment their neighbours change
  if (is(name, 'observer', 'piston', 'sticky_piston', 'dispenser', 'dropper', 'crafter', 'redstone_block', /redstone_(wall_)?torch$/, 'lever', 'daylight_detector', 'target')) return 'sensitive'
  if (is(name, 'redstone_wire', 'repeater', 'comparator', /_button$/, /_pressure_plate$/, /(^|_)rail$/, 'tripwire_hook', 'note_block', 'hopper', /(^|_)(wall_)?torch$/, /_carpet$/, 'ladder', /_sign$/)) return 'component'
  return 'structure'
}

// The order phases are built in. Structure first so components have something to sit on;
// things that react to block updates near the end; TNT dead last so nothing primes it mid-build.
const PHASES = ['structure', 'component', 'sensitive', 'fluid', 'tnt']

/**
 * @returns {{
 *   skip?: string, item: string, category: string,
 *   attempts: Array<{ refDirs: string[], look: string|null, half?: 'top'|'bottom' }>,
 *   clicks: number, check: string[], doubleSlab?: boolean, bucket?: boolean
 * }}
 */
function placementFor (name, props = {}) {
  const facing = props.facing
  const base = { item: itemNameFor(name), category: category(name), clicks: 0, check: [] }

  if (SKIP.has(name)) return { ...base, skip: 'placed automatically or not placeable' }
  if ((name === 'water' || name === 'lava') && props.level && props.level !== '0') return { ...base, skip: 'flowing liquid' }
  if (props.half === 'upper' && /(door|tall_grass|large_fern|sunflower|lilac|rose_bush|peony|pitcher_plant|small_dripleaf)$/.test(name)) return { ...base, skip: 'upper half is placed with the lower half' }
  if (props.part === 'head' && /_bed$/.test(name)) return { ...base, skip: 'head is placed with the foot' }
  if (name.startsWith('potted_')) return { ...base, skip: 'potted plants need the pot and plant combined by hand' }

  const p = (attempts, extra = {}) => ({ ...base, attempts, ...extra })
  const one = (refDirs, look = null, half) => [{ refDirs, look, half }]
  // the look direction we expect, then the opposite in case this server/version disagrees
  // (every placement is checked afterwards, so a wrong guess just costs a retry)
  const flip = (refDirs, look) => [{ refDirs, look }, { refDirs, look: OPPOSITE[look] }]

  // ---- fluids
  if (name === 'water' || name === 'lava') return p(one(ALL), { bucket: true })

  // ---- diodes: output points away from the player, so look the opposite of "facing"
  if (name === 'repeater') {
    return p(flip(['down'], OPPOSITE[facing]), { clicks: (Number(props.delay) || 1) - 1, check: ['facing', 'delay'] })
  }
  if (name === 'comparator') {
    return p(flip(['down'], OPPOSITE[facing]), { clicks: props.mode === 'subtract' ? 1 : 0, check: ['facing', 'mode'] })
  }

  // ---- observer: its face points the way the player looks (incl. up/down)
  if (name === 'observer') return p(flip(ALL, facing), { check: ['facing'] })

  // ---- pistons and friends point back at the player
  if (is(name, 'piston', 'sticky_piston', 'dispenser', 'dropper', 'barrel', /command_block$/)) {
    return p(flip(ALL, OPPOSITE[facing]), { check: ['facing'] })
  }

  // ---- hopper: points into the block you clicked (or down if you clicked top/bottom)
  if (name === 'hopper') {
    if (facing === 'down') return p(one(['down', 'up']), { check: ['facing'] })
    return p(one([facing]), { check: ['facing'] })
  }

  // ---- things stuck to the side of a block: the block behind them is the opposite of facing
  if (/(^|_)wall_torch$/.test(name) || is(name, 'ladder', 'tripwire_hook', /_wall_sign$/, /_wall_banner$/, /_wall_head$/, /_wall_skull$/, /_wall_fan$/)) {
    return p(one([OPPOSITE[facing]]), { check: ['facing'] })
  }
  if (is(name, 'end_rod', 'lightning_rod', /amethyst_(cluster|bud)$/)) {
    return p(one([OPPOSITE[facing]]), { check: ['facing'] })
  }

  // ---- buttons and levers
  if (/_button$/.test(name) || name === 'lever') {
    const check = ['face', 'facing']
    if (props.face === 'floor') return p(one(['down'], facing), { check })
    if (props.face === 'ceiling') return p(one(['up'], facing), { check })
    return p(one([OPPOSITE[facing]]), { check })
  }

  // ---- stairs: face the way the player looks; half picked by where you click
  if (/_stairs$/.test(name)) {
    const top = props.half === 'top'
    return p([
      { refDirs: [top ? 'up' : 'down'], look: facing },
      { refDirs: HORIZONTAL, look: facing, half: top ? 'top' : 'bottom' }
    ], { check: ['facing', 'half'] })
  }

  // ---- slabs
  if (/_slab$/.test(name)) {
    if (props.type === 'top') return p([{ refDirs: ['up'], look: null }, { refDirs: HORIZONTAL, look: null, half: 'top' }], { check: ['type'] })
    const attempts = [{ refDirs: ['down'], look: null }, { refDirs: HORIZONTAL, look: null, half: 'bottom' }]
    if (props.type === 'double') return p(attempts, { check: ['type'], doubleSlab: true })
    return p(attempts, { check: ['type'] })
  }

  // ---- trapdoors: click the side of the block behind them; half by click height
  if (/_trapdoor$/.test(name)) {
    const half = props.half === 'top' ? 'top' : 'bottom'
    return p(one([OPPOSITE[facing]], null, half), { check: ['facing', 'half'], clicks: props.open === 'true' && name !== 'iron_trapdoor' ? 1 : 0 })
  }

  // ---- doors / fence gates face the way the player looks
  if (/_door$/.test(name)) return p(one(['down'], facing), { check: ['facing'], clicks: props.open === 'true' && name !== 'iron_door' ? 1 : 0 })
  if (/_fence_gate$/.test(name)) return p(one(ALL, facing), { check: ['facing'], clicks: props.open === 'true' ? 1 : 0 })

  // ---- note blocks: each right-click raises the pitch by one
  if (name === 'note_block') return p(one(ALL), { clicks: Number(props.note) || 0, check: ['note'] })

  if (name === 'daylight_detector') return p(one(['down']), { clicks: props.inverted === 'true' ? 1 : 0, check: ['inverted'] })

  // ---- things that need a block underneath
  if (is(name, 'redstone_wire', /(^|_)rail$/, /_carpet$/, /_pressure_plate$/, 'torch', 'soul_torch', 'redstone_torch', 'copper_torch', 'lily_pad', /_sapling$/, 'snow', /^(potted_)?.*_(tulip|orchid)$/)) {
    return p(one(['down']))
  }

  // ---- pillars (logs, basalt, chains...): axis follows the clicked face
  if (props.axis && AXIS_DIRS[props.axis]) return p(one(AXIS_DIRS[props.axis]), { check: ['axis'] })

  // ---- horizontal "faces you" blocks (furnaces, chests, pumpkins...)
  if (facing && HORIZONTAL.includes(facing) && is(name, /furnace$/, 'smoker', /chest$/, 'carved_pumpkin', 'jack_o_lantern', 'lectern', 'loom', 'beehive', 'bee_nest', 'stonecutter', 'grindstone', /anvil$/, 'chiseled_bookshelf', 'decorated_pot', 'vault', 'trial_spawner')) {
    // anvils/grindstones/stonecutters are rotated 90deg or face away - the check+retry sorts out which
    return p([{ refDirs: ALL, look: OPPOSITE[facing] }, { refDirs: ALL, look: facing }], { check: ['facing'] })
  }

  // ---- anything else with a facing we don't have a rule for: try both ways and check
  if (facing) {
    return p([{ refDirs: ALL, look: OPPOSITE[facing] }, { refDirs: ALL, look: facing }], { check: ['facing'] })
  }

  return p(one(ALL))
}

// mineflayer yaw: 0 = north, increases counter-clockwise; pitch: + is up
function lookAngles (dir) {
  switch (dir) {
    case 'north': return { yaw: 0, pitch: 0 }
    case 'west': return { yaw: Math.PI / 2, pitch: 0 }
    case 'south': return { yaw: Math.PI, pitch: 0 }
    case 'east': return { yaw: -Math.PI / 2, pitch: 0 }
    case 'up': return { yaw: 0, pitch: Math.PI / 2 * 0.98 }
    case 'down': return { yaw: 0, pitch: -Math.PI / 2 * 0.98 }
    default: return null
  }
}

module.exports = { placementFor, itemNameFor, category, lookAngles, PHASES, DIRS, OPPOSITE, INTERACTIVE }
