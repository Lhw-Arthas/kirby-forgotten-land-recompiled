import assert from 'node:assert/strict';
import { test } from 'node:test';
import { primaryAction } from './action.js';

const base = { running: false, phase: 'idle', game_dir: 'D:\dump', game_error: null, tools_missing: [], built: true, error: '' };

test('primary action follows the setup state', () => {
  assert.equal(primaryAction(null).busy, true);
  assert.equal(primaryAction({ ...base, game_dir: null }).act, 'pick');
  assert.equal(primaryAction({ ...base, game_error: 'another game' }).act, 'pick');
  assert.equal(primaryAction({ ...base, built: false }).act, 'setup');
  assert.equal(primaryAction({ ...base, built: false, tools_missing: ['MSYS2 clang'] }).act, 'go-setup');
  assert.equal(primaryAction({ ...base, built: false, phase: 'compile' }).busy, true);
  assert.equal(primaryAction({ ...base, built: false, phase: 'failed', error: 'boom' }).detail, 'boom');
  assert.equal(primaryAction(base).act, 'play');
  assert.equal(primaryAction({ ...base, running: true }).act, null);
});
