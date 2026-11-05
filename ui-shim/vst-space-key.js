import { isPlainSpaceKeyEvent, needsSpaceDefault } from './js/utils/space-key-guard.js';

export function installVstSpaceKeyHandler(windowRef = window) {
  let spaceDown = false;
  let shiftDown = false;
  const forward = async (down, shift) => {
    try {
      if (await windowRef.vst_forwardHostSpace(down, shift) !== true) {
        console.error('The Space key could not reach the host editor.');
      }
    } catch (error) {
      console.error('Could not forward the Space key to the host:', error);
    }
  };

  // Run after target and document handlers. Cancelling scrolling in capture
  // would make defaultPrevented indistinguishable from a real UI action.
  windowRef.addEventListener('keydown', event => {
    if (!isPlainSpaceKeyEvent(event) || needsSpaceDefault(event.target) || event.defaultPrevented) return;
    event.preventDefault();
    if (!event.isTrusted || event.repeat || spaceDown) return;
    spaceDown = true;
    shiftDown = !!event.shiftKey;
    void forward(true, shiftDown);
  });

  // Complete a forwarded press even if focus or modifiers changed meanwhile.
  windowRef.addEventListener('keyup', event => {
    if (!spaceDown || !event.isTrusted || (event.key !== ' ' && event.key !== 'Spacebar')) return;
    spaceDown = false;
    void forward(false, !!event.shiftKey);
  }, true);
  windowRef.addEventListener('blur', () => {
    if (!spaceDown) return;
    spaceDown = false;
    void forward(false, shiftDown);
  });
}
