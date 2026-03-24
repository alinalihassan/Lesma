const splitPath = (fileName: string) => {
  const parts = fileName.split('/')
  const baseName = parts.pop()!

  return [baseName, parts.pop()]
}

const fileExtension = (fileName: string) => {
  const parts = fileName.split('.')
  return parts.pop()
}

const defaultLesmaStub = `print("Hello World")
`

export const newEmptyFileContent = (fileName: string) => {
  const [baseName] = splitPath(fileName)
  const fileExt = fileExtension(baseName!)

  if (fileExt?.toLowerCase() === 'les') {
    return defaultLesmaStub
  }

  return ''
}
