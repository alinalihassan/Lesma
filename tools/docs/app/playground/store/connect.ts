import { connect as reduxConnect } from 'react-redux'
import { type State } from './state'

/**
 * Type-safe wrapper around redux connect with prepared state types
 *
 * @param mapStateToProps
 */
export const connect =
  <StateProps, OwnProps extends object = Record<string, never>>(mapStateToProps: (state: State) => StateProps) =>
  // Wrapped components receive injected `dispatch` and merged props; typing that precisely duplicates react-redux.
  // eslint-disable-next-line @typescript-eslint/no-explicit-any
  (component: any) => reduxConnect<StateProps, any, OwnProps, State>(mapStateToProps)(component)
