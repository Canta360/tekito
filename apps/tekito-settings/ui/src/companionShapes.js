// The companion's shapes, for Home (mascot.jsx) and the desktop
// (apps/tekito-settings/Companion.cpp, from CompanionShapes.h). After a
// change: node write-companion-shapes.mjs, which writes that header and the
// eyes as files in assets/icons/companion/.

// The body: TEKITO's filled mark (assets/icons/symbols/tekito-filled.svg)
// without the T: a disc (center 450, 450; radius 362), its lower half a
// little flattened so it does not look drawn with compasses, with room cut
// around its small ring, the ring a little smaller than the mark's (86
// across the outside, a 35 hole; the mark's 108 and 44). Drawn in
// "60 60 800 800".
export const body = {
  disc: "M88 450A362 362 0 0 1 812 450C812 640 680 768 450 768C220 768 88 640 88 450Z",
  cut: { cx: 709, cy: 695, r: 120 },
  ring: { cx: 709, cy: 695, outer: 86, hole: 35 },
  // The app icon's colors (assets/icons/Tekito.ico): light blue at the top
  // left, blue, cyan at the bottom right, along the body's box from (0.1,
  // 0.05) to (0.9, 0.95); a soft white sheen at the top left; the small
  // ring cyan from its top left to its bottom right.
  stops: [[0, "#a6c8ff"], [0.3, "#3f88ff"], [0.55, "#0668ff"], [0.75, "#0590ff"], [1, "#00dcff"]],
  ringStops: [[0, "#6fe8ff"], [1, "#10b6fe"]],
  sheen: { cx: 0.18, cy: 0.14, r: 0.62, opacity: 0.6 },
  // The blue's hue, which Rainbow turns to the accent color's.
  blueHue: 216,
};

// Where the eyes go, and how much larger than drawn below: a little up and
// to the left of the middle, so the face is not set too squarely.
export const face = { eyes: [268, 572], y: 404, scale: 1.15 };

// The eyes, from the user's drawings. Each is the left eye, centered on 0, 0
// and about 200 across; the right eye is its mirror image.
export const eyeShapes = {
  // Open: a round eye, a little square.
  open: { fill: "M-100 0C-100 -52 -62 -84 0 -84C62 -84 100 -52 100 0C100 52 62 84 0 84C-62 84 -100 52 -100 0Z" },
  // Pleased: the upper half, flat below.
  happy: { fill: "M-100 34C-100 -36 -58 -76 0 -76C58 -76 100 -36 100 34Q100 44 90 44H-90Q-100 44 -100 34Z" },
  // Half shut: the lower half under a level lid.
  half: { fill: "M-100 -34Q-100 -44 -90 -44H90Q100 -44 100 -34C100 34 58 64 0 64C-58 64 -100 34 -100 -34Z" },
  // Sad: the lower half under a lid that rises toward the middle.
  sad: { fill: "M-100 -12Q-100 -22 -90 -24L88 -52Q100 -54 100 -42C100 30 58 64 0 64C-58 64 -100 38 -100 -12Z" },
  // Shut: a line, higher on the outside.
  shut: { line: "M-96 -24Q-10 -40 92 12" },
};

// A drop under the sad right eye (placed at the right eye, not mirrored):
// filled, and as a line hanging from the eye's edge.
export const tear = "M-46 46C-44 70 -50 84 -50 96A20 20 0 0 0 -10 96C-10 84 -16 70 -14 46Z";
export const tearLine = "M-45 60C-44 72 -50 84 -50 96A20 20 0 0 0 -10 96C-10 84 -15 72 -14 63";
