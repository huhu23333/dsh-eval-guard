/** Shell capabilities published to the Settings panel. */
export interface EvalWorkspace { id: string; label: string }
export interface EvalBridge {
  startSession(workspaceId: string, prompt: string): void
  workspaces(): readonly EvalWorkspace[]
}
let current: EvalBridge | undefined
export function configureEvalBridge(next: EvalBridge | undefined): void { current = next }
export function evalBridge(): EvalBridge | undefined { return current }
