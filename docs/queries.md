# Queries: asking the world anything about a place

Generation, the server, clients and tools keep needing questions about places that the formed
world doesn't answer directly. How far is the sea? Which way does the slope face? Is there a
river within a kilometre? Where is the nearest flat ground by a lake, out of the wind? Palms want
the first: they need salt and coasts. So does every later layer: animals, settlements, quests,
GM tools. Rather than a one-off reading for each, there is one way to ask, made of parts that
combine.

## Decided

- **Fields, not one-off readings.** Everything a question can name is a field over the planet: a
  value at any place, at any detail. Height, warmth, rain, flow, soil, cover and each kind's
  share, distance to the sea. A field also gives **bounds over an area**: the least and most it
  takes anywhere within a node. Bounds are what make searching fast, because a whole node can be
  passed over when its bounds can't hold an answer.
- **Questions are expressions over fields.** Arithmetic, min and max, the fades the woods use
  (`rise`, `band`), and comparisons that give shares, not yes or no, as everywhere else
  ([blend boundaries](world-v3.md)). An expression is a field too, so its bounds come from its
  parts' by interval arithmetic (`globe::Interval`). Any expression can be searched.
- **Typed C++ first, text later.** Expressions are built in C++ as a small tree of typed nodes,
  checked when compiled. A text form of the same tree, parsed into it, comes once something
  outside C++ needs to ask: the console, GM tools, Squirrel scripts. That makes it one language,
  not two.
- **The same everywhere.** Portable maths only, ties broken by place, so the generator, the
  server and clients get the same answer to the same question
  as all generation is.
- **Where the answer comes from differs, the question doesn't.** The generator answers from the
  formed world at any detail. The server answers from its pages and the edits over them, so a
  felled forest reads as felled. Clients answer from the pages they hold, as deep as they hold
  them. A field names what it reads, and each place that answers says which fields it has.

## What's missing to answer them

Most fields are already readings of the formed world or the pages. The ones that aren't are
**distances**: to the sea, a lake, a river of some size, ice, a kind of ground. They can't be
read at a place, since the answer lies somewhere else. They're worked out once, when the world
forms:

- spread over the formed cells outward from what they measure to, along the ground, so a ridge
  between counts its climb and a strait doesn't count as land;
- held with the formed world, coarse, and given finely near what they measure to from its exact
  shape (the water bodies' shores);
- with bounds per node, like every field.

## Steps

1. **The fields and their bounds.** A field interface: value at a place and detail, bounds over
   a node. It wraps what's read now: the habitat's parts and the woods. Tests: each field's
   value at a place lies within its bounds over every node holding the place; bounds narrow as
   nodes shrink.
2. **Distance to the sea**, then to lakes and rivers by flow, spread over the formed cells and
   refined at the shore. Tests: none at the shoreline, rising by about a metre per metre along
   the ground; coarse and fine agree; a peninsula's tip is near the sea, a basin behind a ridge
   far from it. With it, **salt** for the habitat (spray falling off over a few hundred metres
   inland, more on windward coasts), and palms and mangroves tied to coasts by it.
3. **Expressions**: the typed tree with arithmetic, fades and comparisons, its bounds by
   interval arithmetic. Tests: an expression's value equals its parts' combined directly; its
   bounds hold every value inside them.
4. **Searches**: the nearest place, all places within an area, or a spread sample where an
   expression holds at least so much, going down the formed cells or the pages and passing over
   nodes whose bounds rule them out. Deterministic order, with a budget on the work done. Tests
   against brute force over small areas; how many nodes are passed over.
5. **Answering on the server and clients** from pages and edits, for the fields they carry.
   The census moves onto queries.
6. **The text form**, once the console, GM tools or scripts need it. It parses into the same
   tree, so it brings no new meaning.

## Open

- Which questions come first besides the sea: aspect and wind exposure, distance to cliffs and
  caves, "flat enough to build on"? Steps 1–2 are enough for palms. The rest can come as their
  users do.
- Whether GM-made meta areas are fields in this sense
  ([planet layers](planet-layers.md), item 12, which this plan carries out). They likely are,
  which would let one question mix the natural world with the rules laid over it.
