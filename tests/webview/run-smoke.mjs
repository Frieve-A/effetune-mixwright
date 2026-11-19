import { spawnSync } from 'node:child_process';
import { mkdtemp, rm, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import path from 'node:path';
import { createSpeakerModel } from '../../external/effetune/tests/helpers/speaker-model-fixture.mjs';
import { encodeSpeakerModel } from '../../external/effetune/features/measurement/dataStorage.js';

const temporary = await mkdtemp(path.join(tmpdir(), 'effetune-speaker-test-'));
try {
  let measurement = process.env.EFFETUNE_TEST_DISTORTION_MEASUREMENT;
  if (!measurement) {
    measurement = path.join(temporary, 'speaker.json');
    await writeFile(measurement, JSON.stringify({
      name: 'Test speaker', points: [], speakerModel: encodeSpeakerModel(createSpeakerModel(1))
    }));
  }
  const result = spawnSync(process.argv[2], ['--distortion-measurement', measurement],
    { stdio: 'inherit', windowsHide: true });
  if (result.error) throw result.error;
  process.exitCode = result.status ?? 1;
} finally {
  await rm(temporary, { recursive: true, force: true });
}
