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
- **Gather raw materials by itself.** It builds from chests near the build site.
  Fill them yourself (run `npm run plan` for the exact shopping list), or pair it with
  a Mindcraft gatherer bot (see [Gathering materials with Mindcraft](#gathering-materials-with-mindcraft)).
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

## Gathering materials with Mindcraft

CannonBot only builds; it doesn't gather. [Mindcraft](https://github.com/kolbytn/mindcraft)
is a separate, AI-driven Mineflayer bot that can chop, mine, smelt and craft. The two
work as a pair: Mindcraft fills the supply chest, and CannonBot builds from it.

1. Install Mindcraft next to this folder (it needs Node 18/20):
   ```
   git clone https://github.com/kolbytn/mindcraft ../mindcraft
   cd ../mindcraft && npm install
   ```
2. In the Mindcraft folder, copy `keys.example.json` to `keys.json` and add your
   `ANTHROPIC_API_KEY` (or set it as an environment variable). Mindcraft calls the
   model constantly, so expect API costs to add up over a long gathering session.
   `"mindcraft": { "model": "claude-sonnet-5-5" }` is a cheaper option.
3. In your `config.json`, set `origin` (where the cannon goes), put a chest next to the
   build site in `chests` (or `mindcraft.dropOff`), and set `"retryMinutes": 5`.
4. Start both, in two terminals:
   ```
   npm run mindcraft     # the gatherer
   npm start             # CannonBot
   ```

`npm run mindcraft` turns the cannon's material list into a goal for the gatherer
("collect or craft 49 smooth_stone, 2 repeater, ... and put them in the chest at x y z;
stay off the build site; don't break anything players built"). It writes Mindcraft's
profile and settings to `mindcraft-setup/`, then launches Mindcraft with them, so you
don't need to edit Mindcraft's own files. CannonBot builds what it can; when it runs short,
it checks the chests again every `retryMinutes` and carries on as the deliveries arrive.

Things to know:
- Mindcraft is an AI agent, not a script. It's good at "get 20 cobblestone" or "craft a
  furnace", and much less reliable at long crafting chains such as quartz for
  comparators and observers (that means a trip to the Nether). Keep an eye on it, and
  top up anything it struggles with yourself. Give it a smaller schematic first.
- It doesn't handle TNT. Keep `"skipBlocks": ["tnt"]` and the gatherer won't be asked for it.
- With `owner` set, it only talks to you (whispers), like CannonBot.
- It supports Minecraft up to 1.21.11 (1.21.6 recommended by its authors).

### Forge and other modded servers

Neither bot works on a typical Forge/NeoForge modpack server. Both are built on Mineflayer,
which speaks the **vanilla** protocol. Modded servers that require mods on the client
reject vanilla clients at login, and modded blocks/items aren't in Mineflayer's data even
when one does get in. What does work:
- servers whose mods are **server-side only** and let vanilla clients join
  (Paper/Spigot plugins, server-side Fabric mods such as Lithium);
- vanilla servers, LAN worlds and Realms-style setups on a supported version.

If your friends' server is a Forge modpack, you'd need a vanilla (or plugin-only)
server for the bots.

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
| `retryMinutes` | `0` | When the build is short on materials, check the chests again every this many minutes (use with Mindcraft) |
| `mindcraft.path` | – | Your Mindcraft folder. `npm run mindcraft` starts it from there |
| `mindcraft.username` | `Gatherer` | The Mindcraft bot's name (must differ from `username`) |
| `mindcraft.dropOff` | first of `chests` | `[x, y, z]` of the chest the gatherer delivers to |
| `mindcraft.model` | Claude Opus 5.5 | Any Mindcraft `model` value, e.g. `"claude-sonnet-5-5"` to spend less |
| `mindcraft.settings` | – | Extra Mindcraft `settings.js` values to override |

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
src/mindcraft.js   Mindcraft gatherer setup
src/index.js       CLI, config, chat commands
```

Facing is the hard part in survival. A block's direction comes from where the
player is looking and which face they click. The bot turns to the right
direction, sneak-clicks the right neighbouring block, then reads the result back
from the world. If it's wrong (some blocks differ between versions), it breaks
the block, picks it back up and tries the other way.

Run the tests with `npm test`.
