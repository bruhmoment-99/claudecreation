'use strict'
// Turns a loaded schematic into an ordered build list plus a material list.

const { placementFor, PHASES } = require('./placement')

/**
 * @param {{ blocks: Array<{x,y,z,name,props}> }} schem
 * @param {object|null} registry minecraft-data for the server version (null = don't validate names)
 * @param {{ skipBlocks?: string[] }} options
 */
function makePlan (schem, registry, options = {}) {
  const skipBlocks = new Set(options.skipBlocks || [])
  const steps = []
  const skipped = {}
  const unknown = new Set()

  for (const b of schem.blocks) {
    if (skipBlocks.has(b.name)) { bump(skipped, `${b.name} (skipBlocks in config)`); continue }
    const how = placementFor(b.name, b.props)
    if (how.skip) { bump(skipped, `${b.name} (${how.skip})`); continue }
    if (registry && (!registry.blocksByName[b.name] || !registry.itemsByName[how.item])) {
      unknown.add(b.name)
      continue
    }
    steps.push({ x: b.x, y: b.y, z: b.z, name: b.name, props: b.props, how })
  }

  steps.sort(compareSteps)

  const materials = {}
  const fluidSources = {}
  for (const s of steps) {
    if (s.how.bucket) { bump(fluidSources, s.name); continue }
    bump(materials, s.how.item, s.how.doubleSlab ? 2 : 1)
  }
  // One bucket is enough: the bot refills it from the nearest source block outside the build
  for (const fluid of Object.keys(fluidSources)) materials[`${fluid}_bucket`] = 1

  return { steps, materials, fluidSources, skipped, unknown: [...unknown], size: schem.size }
}

// Phase, then bottom-up, then a back-and-forth sweep so the bot doesn't zig-zag across the build
function compareSteps (a, b) {
  const pa = PHASES.indexOf(a.how.category)
  const pb = PHASES.indexOf(b.how.category)
  if (pa !== pb) return pa - pb
  if (a.y !== b.y) return a.y - b.y
  if (a.x !== b.x) return a.x - b.x
  return (a.x % 2 === 0) ? a.z - b.z : b.z - a.z
}

function bump (obj, key, n = 1) { obj[key] = (obj[key] || 0) + n }

function formatMaterials (materials, have = null) {
  const rows = Object.entries(materials).sort((a, b) => b[1] - a[1])
  return rows.map(([item, need]) => {
    const got = have ? have[item] || 0 : 0
    const stacks = need >= 64 ? ` (${Math.floor(need / 64)} stacks + ${need % 64})` : ''
    const status = have === null ? '' : got >= need ? '  ok' : `  MISSING ${need - got}`
    return `  ${String(need).padStart(6)}  ${item}${stacks}${status}`
  }).join('\n')
}

module.exports = { makePlan, formatMaterials, compareSteps }
