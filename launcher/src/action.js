// The one thing the big button does next, from the backend status. Pure, so it can be tested without a window.
export function primaryAction(s) {
  if (!s) return { label: 'Loading', act: null, busy: true };
  if (s.running) return { label: 'Running', act: null, busy: true };
  if (['runtime', 'translate', 'compile'].includes(s.phase)) return { label: 'Setting up', act: 'go-setup', busy: true };
  if (!s.game_dir) return { label: 'Select game folder', act: 'pick' };
  if (s.game_error) return { label: 'Select game folder', act: 'pick', detail: s.game_error };
  if (s.tools_missing.length && !s.built) return { label: 'Set up', act: 'go-setup', detail: `Missing build tools: ${s.tools_missing.join(', ')}.` };
  if (!s.built) return { label: 'Set up', act: 'setup', detail: s.phase === 'failed' ? s.error : '' };
  return { label: 'Play', act: 'play' };
}

export const PHASE_LABEL = {
  runtime: 'Building the runtime',
  translate: 'Translating the game code',
  compile: 'Compiling the game code',
};
