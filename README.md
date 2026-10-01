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
- Hunt down a player and bring you their loot, guard you, or follow you (see [Hunting](#hunting-guarding-and-following)).

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
3. Get an orbital cannon schematic as a `.litematic` or `.schem` file, and put
   it in a `schematics/` folder. See [Where to get a cannon](#where-to-get-a-cannon).
4. Copy `config.example.json` to `config.json` and edit it (see [Config](#config)).
5. Check the material list:
   ```
   npm run plan
   ```
6. Put those materials, plus some food, in chests near where the cannon will go.
   It finds chests within `chestSearchRadius` blocks, or you can list them in
   `chests`. It never takes from chests inside the build area.

## Where to get a cannon

The best-known Java orbital strike cannon is by **cubicmetre** (the
design the "Wemmbu" cannon is based on). These are its 1.21 versions:

- **Orbital Strike Cannon Mk 6.1** (Java 1.21.11, about 58 x 68 x 47, about
  13,500 blocks). This version includes a fix that should make it work across 1.21.x:
  [mineschematic.com](https://mineschematic.com/s/the-orbital-strike-cannon-mk-6-1-40cb8504).
  For how to aim and fire it:
  [How to use the OSC Mk 6.1](https://www.youtube.com/watch?v=7UO334neMgM).
- **Orbital Strike Cannon by Cubicmeter** (Java 1.21.10, 32 x 32 x 32, about 3,200 blocks):
  [mineschematic.com](https://mineschematic.com/s/orbital-strike-cannon-by-cubicmeter-e30ade34)
  or [donut.build](https://donut.build/schematic/orbital-strike-cannon).
- Older versions on Planet Minecraft:
  [Orbital Strike Cannon 4.3 by cubicmetre](https://www.planetminecraft.com/project/orbital-strike-cannon-4-3-by-cubicmetre/).

Avoid "orbital cannon" results on createmod.com. Those need the Create mod and
won't work in vanilla. The datapack and mod versions on Modrinth and CurseForge
aren't builds at all; they add the cannon as an item.

After downloading, run `npm run plan` to see what it needs. A big cannon needs
hundreds of TNT and a lot of other blocks, so gather materials before you start.

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

The bot joins and says hi. Then tell it where to build:

```
build at 120 64 -300
```

That block becomes the build's lowest north-west corner. From there the build
extends toward +X (east), +Y (up) and +Z (south). Use your F3 screen to read
coordinates. You can also:

- use `~` like in Minecraft commands: `build at ~10 ~ ~-5` means 10 blocks east
  and 5 north of where you're standing;
- turn the whole build: `build at 120 64 -300 rotate 90` (clockwise: 90, 180 or 270);
- stand on the spot and say `build here`.

If the spot is far away, the bot walks there first. Say `where` to have it
tell you the exact corners of the build area before it starts digging.

### All commands

Type these in game chat, or in the terminal running the bot. If `owner` is
set, it only listens to you.

| Command | What it does |
|---|---|
| `build at <x> <y> <z> [rotate 90]` | Build with the corner at those coordinates |
| `build here` | Build with the corner at your feet |
| `build` | Start or continue at the last spot (or `origin` from the config) |
| `where` | Show the corners of the build area |
| `pause` / `resume` | Pause and carry on |
| `stop` | Stop whatever it's doing (hunt/guard/follow first, then the build) |
| `status` | Progress report |
| `materials` | List the missing items |
| `hunt <player>` | Hunt that player down, collect what they drop, and bring it to you |
| `deliver` | Hand over any loot it's still carrying |
| `guard` | Follow you and fight hostile mobs near you, plus anyone who hits you |
| `follow` | Just follow you around |
| `come` | Walk to you |
| `help` | List commands in chat |
| `quit` | Disconnect |

When it can't place something (missing items, a block it can't reach), it
finishes everything else, then tells you what's wrong and where. Fix it, for
example by adding the items to a chest, then say `build` again.

**Try it on the small demo first.** Run `npm run demo`, set `"schematic":
"examples/demo.schem"`, and check the bot gets repeaters, observers, pistons and
stairs right on your server before you give it a 13,000-block cannon.

### Hunting, guarding and following

`hunt <player>` makes the bot put on the best armour it's carrying, pick its
best weapon (a fast sword beats a slow axe), and chase that player:

- It uses the pathfinder when the player is far away or on another level, and
  steers straight at them up close.
- Its swings are timed to the weapon's cooldown, so every hit is full strength.
- When the player dies, it picks up everything that dropped nearby, walks back
  to you and throws you the loot. If you're not around, it keeps the loot until
  you say `deliver`.

Things to know:
- It only takes hunting orders from `owner`, and won't hunt you.
- The target has to be within the bot's render distance. A player list name is
  not enough to track someone across the map.
- It backs off when its health drops to `retreatHealth` and gives up after
  `huntMinutes`.
- It won't dig through blocks to reach someone. Hiding in a 1x1 hole works on it.
- PvP has to be on (`pvp=true` in `server.properties`), and players with good
  armour and a shield will beat it. It's a decent fighter, not an aimbot.
- On a server that isn't yours, hunting other players is only OK if the rules allow it.

Hunting or guarding in the middle of a build pauses the build. The bot goes back
to building when the hunt ends or when you say `stop`.

## Config

| Key | Default | Meaning |
|---|---|---|
| `host`, `port` | `localhost`, `25565` | Server address |
| `username` | `CannonBot` | Bot name (offline) or account email (microsoft) |
| `auth` | `offline` | `offline` or `microsoft` |
| `version` | auto | Minecraft version, e.g. `"1.21.4"`. `false` = detect |
| `owner` | – | Your username. Only you can command the bot, and it whispers updates to you |
| `schematic` | – | Path to the `.litematic` / `.schem` file |
| `origin` | `null` | `[x, y, z]` for the build's corner. `null` = wait for a `build at` / `build here` command |
| `rotate` | `0` | Rotate the build clockwise: `0`, `90`, `180` or `270`. Use it to aim the cannon a different way |
| `clearArea` | `true` | Dig out anything in the build area that isn't part of the build |
| `chests` | `[]` | Extra chest positions `[[x,y,z], ...]` to fetch from |
| `chestSearchRadius` | `32` | How far around the build to look for chests |
| `skipBlocks` | `[]` | Block names not to place, e.g. `["tnt"]` if you want to load the TNT yourself |
| `scaffoldBlocks` | dirt, cobblestone… | Blocks the bot may use to pillar up. Keep a stack in a chest. It cleans up any it leaves inside the build |
| `chat` | `true` | Post updates in public chat when `owner` isn't online |
| `retreatHealth` | `6` | Stop fighting at or below this much health (6 = 3 hearts) |
| `huntMinutes` | `5` | Give up a hunt after this long |
| `guardAgainstPlayers` | `true` | In `guard` mode, also fight players who hit you |

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
src/combat.js      hunt / guard / follow
src/commands.js    chat command parsing
src/index.js       CLI, config, chat commands
```

Facing is the hard part in survival. A block's direction comes from where the
player is looking and which face they click. The bot turns to the right
direction, sneak-clicks the right neighbouring block, then reads the result back
from the world. If it's wrong (some blocks differ between versions), it breaks
the block, picks it back up and tries the other way.

Run the tests with `npm test`.
