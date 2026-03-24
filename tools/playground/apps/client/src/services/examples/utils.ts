/**
 * Legacy identifiers kept so older imports do not break; Lesma playground has no go.mod.
 */
export const goModFile = 'go.mod'
export const goModTemplate = ''

export const isProjectRequiresGoMod = (_files: Record<string, string>) => false
