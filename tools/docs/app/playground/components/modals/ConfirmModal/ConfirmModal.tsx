import React from 'react'
import { Stack, DefaultButton, PrimaryButton, DefaultSpacing, type IStackTokens } from '@fluentui/react'

import { Dialog } from '@/playground/components/elements/modals/Dialog/Dialog'
import { DialogActions } from '@/playground/components/elements/modals/DialogActions/DialogActions'

export interface ConfirmProps {
  isOpen?: boolean
  title: string
  message: string
  confirmText?: string
  onResult?: (isConfirmed: boolean) => void
}

const modalStyles = {
  main: {
    maxWidth: 480,
    minHeight: 'none',
  },
}

const verticalStackTokens: IStackTokens = {
  childrenGap: DefaultSpacing.s1,
}

export const ConfirmModal: React.FC<ConfirmProps> = ({ isOpen, title, message, onResult, confirmText }) => (
  <Dialog label={title} isOpen={isOpen} styles={modalStyles} onDismiss={() => onResult?.(false)}>
    <Stack tokens={verticalStackTokens}>
      <Stack.Item>
        <span>{message}</span>
      </Stack.Item>
      <Stack.Item>
        <DialogActions>
          <DefaultButton
            text="Cancel"
            onClick={() => {
              onResult?.(false)
            }}
          />
          <PrimaryButton
            text={confirmText ?? 'OK'}
            autoFocus={true}
            onClick={() => {
              onResult?.(true)
            }}
          />
        </DialogActions>
      </Stack.Item>
    </Stack>
  </Dialog>
)
