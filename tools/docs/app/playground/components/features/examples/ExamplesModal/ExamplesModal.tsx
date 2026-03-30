import React from 'react'
import { Dialog } from '@/playground/components/elements/modals/Dialog/Dialog'
import { getSnippetsList, type Snippet } from '@/playground/services/examples/client'

import { ExamplesSection } from '@/playground/components/features/examples/ExamplesSection/ExamplesSection'

interface Props {
  isOpen?: boolean
  onDismiss?: () => void
  onSelect?: (snippet: Snippet) => void
}

const examples = getSnippetsList()

const modalStyles = {
  main: {
    maxWidth: 840,
  },
}

export const ExamplesModal: React.FC<Props> = ({ isOpen, onDismiss, onSelect }) => {
  return (
    <Dialog label="Examples" styles={modalStyles} isOpen={isOpen} onDismiss={onDismiss}>
      {Object.entries(examples).map(([label, snippets]) => (
        <ExamplesSection key={label} label={label} snippets={snippets} onSelect={onSelect} />
      ))}
    </Dialog>
  )
}
