export function formatEventTime(value: string): string {
  if (/^\d+$/.test(value)) {
    return `${value}s`;
  }

  const parsed = new Date(value);
  if (Number.isNaN(parsed.getTime())) {
    return value;
  }
  return parsed.toLocaleTimeString();
}
