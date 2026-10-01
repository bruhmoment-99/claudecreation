# CannonBot

A bot that joins your Minecraft **Java Edition** world as a player and builds a
schematic for you in **survival**. Point it at an orbital TNT cannon schematic,
fill a few chests with the materials, and it does the rest: it clears the area,
fetches blocks from the chests, walks and pillars up to every spot, places each
block facing the right way, sets repeater delays and comparator modes, checks
every block after placing it, and redoes anything that came out wrong.

It's built on [Mineflayer](https://github.com/PrismarineJS/mineflayer) and
[mineflayer-pathfinder](https://github.com/PrismarineJS/mineflayer-pathfinder).

## What it does and doesn't do

**It does:**
- Read `.litematic` (Litematica) and `.schem` (WorldEdit/Sponge) files.
- Take materials out of chests and barrels near the build, and drop off any junk it picked up.
- Clear whatever's in the build area first.
- Get directional blocks right: repeaters, comparators, observers, pistons,
  dispensers, hoppers, wall torches, levers, buttons, stairs, slabs, trapdoors and logs.
- Set repeater delay, comparator mode and note block pitch.
- Place water and lava sources with a bucket, refilling it from a nearby source.
- Build in phases: structure, then redstone, then observers and pistons, then
  liquids, then **TNT last**. That way nothing goes off halfway through the build.
- Eat when it gets hungry, if it has food.
- Resume where it left off. Blocks that are already correct are skipped.

**It doesn't:**
- **Gather raw materials.** It won't mine obsidian or farm creepers for gunpowder.
  You have to put the materials in chests near the build site. Run
  `npm run plan` first to get the exact shopping list.
- **Come with a cannon design.** Orbital cannons depend on the Minecraft version
  and use very specific designs. Download a schematic made for **your exact
  version** (see below).
- Work on Bedrock Edition, consoles or Pocket Edition. It's Java Edition only.

## Setup

1. Install [Node.js](https://nodejs.org) 18 or newer.
2. In this folder, run:
   ```
   npm install
   ```
3. Get an orbital cannon schematic as a `.litematic` or `.schem` file. They're
   shared on Planet Minecraft, in YouTube video descriptions, and on technical
   Minecraft Discords. Make sure it says it works on your Minecraft version
   (TNT and redstone behaviour changes between versions). Put it in a
   `schematics/` folder.
4. Copy `config.example.json` to `config.json` and edit it (see [Config](#config)).
5. Check the material list:
   ```
   npm run plan
   ```
6. Put those materials, plus some food, in chests near where the cannon will go.
   It finds chests within `chestSearchRadius` blocks, or you can list them in
   `chests`. It never takes from chests inside the build area.

## Letting the bot join your world

The bot is a separate player, so it needs a way in:

- **Your own server** (vanilla, Paper, etc.): set `online-mode=false` in
  `server.properties` and use `"auth": "offline"` with any username. If you'd
  rather keep online mode on, use `"auth": "microsoft"` with a second Minecraft
  account. The first time it runs, it prints a code to sign in with.
- **Single-player:** pause, choose *Open to LAN*, and set `port` in `config.json`
  to the port number shown in chat. LAN worlds check accounts, so the bot needs
  `"auth": "microsoft"` with a **second** account. An offline bot can't join.
  The easier option is a local server: download the server jar from
  minecraft.net, copy your world into it, and set `online-mode=false`.
- On someone else's server, check the rules first. Most public servers ban bots.

## Running it

```
npm start
```

The bot joins and says hi. If `origin` isn't set in `config.json`, **stand where
you want the build's corner to be and type `build here` in chat.** The
schematic's lowest north-west corner goes on that block. The build extends
toward +X (east), +Y (up) and +Z (south) from there.

Commands (type them in game chat, or in the terminal running the bot):

| Command | What it does |
|---|---|
| `build here` | Start building with the corner at your feet |
| `build` | Start or continue at the configured origin |
| `pause` / `resume` | Pause and carry on |
| `stop` | Stop after the current block |
| `status` | Progress report |
| `materials` | List the missing items |
| `come` | Walk to you |
| `quit` | Disconnect |

When it can't place something (missing items, a block it can't reach), it
finishes everything else, then tells you what's wrong and where. Fix it, for
example by adding the items to a chest, then say `build` again.

**Try it on the small demo first.** Run `npm run demo`, set `"schematic":
"examples/demo.schem"`, and check the bot gets repeaters, observers, pistons and
stairs right on your server before you give it a 5,000-block cannon.

## Config

| Key | Default | Meaning |
|---|---|---|
| `host`, `port` | `localhost`, `25565` | Server address |
| `username` | `CannonBot` | Bot name (offline) or account email (microsoft) |
| `auth` | `offline` | `offline` or `microsoft` |
| `version` | auto | Minecraft version, e.g. `"1.21.4"`. `false` = detect |
| `owner` | – | Your username. Only you can command the bot, and it whispers updates to you |
| `schematic` | – | Path to the `.litematic` / `.schem` file |
| `origin` | `null` | `[x, y, z]` for the build's corner. `null` = wait for `build here` |
| `rotate` | `0` | Rotate the build clockwise: `0`, `90`, `180` or `270`. Use it to aim the cannon a different way |
| `clearArea` | `true` | Dig out anything in the build area that isn't part of the build |
| `chests` | `[]` | Extra chest positions `[[x,y,z], ...]` to fetch from |
| `chestSearchRadius` | `32` | How far around the build to look for chests |
| `skipBlocks` | `[]` | Block names not to place, e.g. `["tnt"]` if you want to load the TNT yourself |
| `scaffoldBlocks` | dirt, cobblestone… | Blocks the bot may use to pillar up. Keep a stack in a chest. It cleans up any it leaves inside the build |
| `chat` | `true` | Post updates in public chat when `owner` isn't online |

## Safety

- **Back up your world first.** It's a TNT cannon.
- TNT goes in last, but a mis-oriented observer or a schematic saved mid-firing
  could still prime something. Stay out of the blast area while it builds, and
  check the redstone before you fire.
- The bot builds exactly what the schematic says. If the design doesn't work on
  your version, the build won't either.

## How it works

```
src/schematic.js   reads .litematic / .schem into a block list
src/transform.js   rotation
src/placement.js   per-block rules: which item, what to click, which way to look
src/planner.js     build order and material list
src/builder.js     the bot: chests, clearing, pathfinding, placing, checking
src/index.js       CLI, config, chat commands
```

Facing is the hard part in survival. A block's direction comes from where the
player is looking and which face they click. The bot turns to the right
direction, sneak-clicks the right neighbouring block, then reads the result back
from the world. If it's wrong (some blocks differ between versions), it breaks
the block, picks it back up and tries the other way.

Run the tests with `npm test`.
