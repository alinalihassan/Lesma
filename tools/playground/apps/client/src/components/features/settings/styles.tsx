import { FontWeights, FontSizes, mergeStyleSets } from '@fluentui/react'

export const settingsPropStyles = mergeStyleSets({
  title: {
    fontWeight: FontWeights.bold,
  },
  container: {
    marginTop: '10px',
  },
  block: {
    marginBottom: '20px',
    '&:first-child': {
      marginTop: '15px',
    },
  },
  description: {
    marginTop: '5px',
  },
})
