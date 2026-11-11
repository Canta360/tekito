// The companion's eyes, from the user's drawings. Each is the left eye,
// centered on 0, 0 and about 200 across; the right eye is its mirror image.
// The same shapes as files: assets/icons/companion/ (written by
// apps/tekito-settings/ui/write-companion-eyes.mjs).
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
