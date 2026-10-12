/** A single-line progress bar on stderr, so stdout stays clean for piping URLs. */
export class ProgressBar {
  readonly #label: string;
  readonly #write: (text: string) => void;
  #lastRendered = -1;
  #lineLength = 0;

  constructor(label: string, write: (text: string) => void) {
    this.#label = label;
    this.#write = write;
  }

  update(sent: number, total: number): void {
    if (total <= 0) return;
    const permille = Math.floor((Math.min(sent, total) * 1000) / total);
    // Redraw at most once per 0.1% so fast uploads don't flood the terminal.
    if (permille === this.#lastRendered) return;
    this.#lastRendered = permille;

    const width = 24;
    const filled = Math.floor((permille * width) / 1000);
    const bar = ("=".repeat(filled) + (filled < width ? ">" : "")).padEnd(width);
    const percent = (permille / 10).toFixed(1).padStart(5);
    const line = `${this.#label} [${bar}] ${percent}% ${formatBytes(sent)}/${formatBytes(total)}`;
    // Pad over the previous line instead of using ANSI erase codes, which older Windows consoles lack.
    this.#write(`\r${line.padEnd(this.#lineLength)}`);
    this.#lineLength = line.length;
  }

  finish(): void {
    if (this.#lineLength > 0) this.#write(`\r${" ".repeat(this.#lineLength)}\r`);
    this.#lineLength = 0;
  }
}

export function formatBytes(count: number): string {
  const units = ["B", "KB", "MB", "GB"];
  let value = count;
  let unit = 0;
  while (value >= 1024 && unit < units.length - 1) {
    value /= 1024;
    unit += 1;
  }
  return unit === 0 ? `${count} B` : `${value.toFixed(1)} ${units[unit]}`;
}
