'use strict'
// Rotate a loaded schematic clockwise (seen from above) in 90 degree steps,
// fixing up direction-dependent block properties as we go.

const CW = { north: 'east', east: 'south', south: 'west', west: 'north' }

function turnWord (word, turns) {
  let w = word
  for (let i = 0; i < turns; i++) w = CW[w] || w
  return w
}

function rotateProps (props, turns) {
  const out = {}
  for (const [key, value] of Object.entries(props)) {
    let k = key
    let v = value
    if (CW[key]) k = turnWord(key, turns) // fence/wall/redstone connections: north=side etc.
    if (key === 'facing' || key === 'horizontal_facing') v = turnWord(value, turns)
    if (key === 'axis' && turns % 2 === 1 && (value === 'x' || value === 'z')) v = value === 'x' ? 'z' : 'x'
    if (key === 'rotation') v = String((Number(value) + 4 * turns) % 16)
    if (key === 'shape' && /(north|south|east|west)/.test(value)) {
      // rails: "north_south", "ascending_east", "south_east"...
      const parts = value.split('_').map(p => turnWord(p, turns))
      // keep vanilla's canonical ordering (north/south first)
      if (parts.length === 2 && CW[parts[0]] && CW[parts[1]]) {
        const order = ['north', 'south', 'east', 'west']
        parts.sort((a, b) => order.indexOf(a) - order.indexOf(b))
      }
      v = parts.join('_')
    }
    out[k] = v
  }
  return out
}

function rotateSchematic (schem, degrees) {
  const turns = (((Math.round(degrees / 90)) % 4) + 4) % 4
  if (turns === 0) return schem
  let { size } = schem
  let blocks = schem.blocks
  for (let t = 0; t < turns; t++) {
    const sz = size.z
    blocks = blocks.map(b => ({ ...b, x: sz - 1 - b.z, z: b.x }))
    size = { x: size.z, y: size.y, z: size.x }
  }
  blocks = blocks.map(b => ({ ...b, props: rotateProps(b.props, turns) }))
  return { size, blocks }
}

module.exports = { rotateSchematic, rotateProps }
