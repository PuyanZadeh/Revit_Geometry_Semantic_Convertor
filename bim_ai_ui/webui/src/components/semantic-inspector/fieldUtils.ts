// fieldUtils.ts
// Backend field casing varies (e.g. "Category" vs "category"); these helpers
// look up whichever variant is present instead of assuming one shape.

export function pickField(obj: any, keys: string[]): any {
  if (!obj || typeof obj !== "object") return undefined;
  for (const k of keys) {
    const v = obj[k];
    if (v !== undefined && v !== null && v !== "") return v;
  }
  return undefined;
}

export function formatCellValue(value: any): string {
  if (value === null || value === undefined) return "";
  if (typeof value === "object") return JSON.stringify(value);
  return String(value);
}
