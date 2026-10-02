import importlib.util
import json
import subprocess
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path

spec = importlib.util.spec_from_file_location('manifest', Path(__file__).resolve().parents[1] / 'tools/vita_build_manifest.py')
manifest = importlib.util.module_from_spec(spec)
spec.loader.exec_module(manifest)


class BuildIdentity(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.repo = self.root / 'repo'
        self.repo.mkdir()
        def git(*args):
            subprocess.run(['git', '-C', str(self.repo), *args], check=True, capture_output=True)
        git('init'); git('config', 'user.email', 'fixture@example.invalid'); git('config', 'user.name', 'Fixture')
        git('config', 'commit.gpgsign', 'false'); git('config', 'core.hooksPath', '/dev/null')
        (self.repo / 'source.cpp').write_text('baseline\n')
        git('add', 'source.cpp'); git('commit', '-m', 'fixture')
        self.build = self.root / 'build'; self.build.mkdir()
        self.cache = self.build / 'CMakeCache.txt'
        self.cache.write_text('AURORA_VITA_ASYNC_GX:BOOL=OFF\nAURORA_VITA_GXM_DIRECT_STREAM_WRITE:BOOL=OFF\nAURORA_VITA_GXM_DIRECT_DRAW_SUBMIT:BOOL=OFF\n')
        self.elf = self.build / 'probe'; self.elf.write_bytes(b'elf fixture')
        self.self_path = self.build / 'probe.self'; self.self_path.write_bytes(b'self fixture')
        self.vpk = self.build / 'probe.vpk'
        self.package(b'self fixture')
    def tearDown(self):
        self.temp.cleanup()
    def package(self, data):
        with zipfile.ZipFile(self.vpk, 'w') as archive:
            archive.writestr('eboot.bin', data)
    def get(self, **kw):
        return manifest.build_manifest(self.repo, self.build, self.elf, self.self_path, self.vpk, **kw)
    def test_deterministic_and_missing_fields(self):
        a = self.get(); self.assertEqual(a, self.get())
        self.assertFalse(a['source']['dirty']); self.assertFalse(a['installed_hash_verified'])
        self.assertTrue(all(value == 'unknown' for value in a['capture'].values()))
        self.assertEqual(a['toolchain']['CMAKE_CXX_COMPILER'], 'unknown')
        self.assertEqual(a['flags']['AURORA_VITA_DISTINCT_CPU_CORES'], 'unknown')
        self.cache.write_text(self.cache.read_text().replace('OFF', 'ON'))
        b = self.get(); self.assertNotEqual(a['cmake'], b['cmake'])
        self.assertTrue(all(value == 'ON' for value in b['cmake'].values()))
    def test_artifact_mismatch_and_missing_path(self):
        self.package(b'wrong'); self.assertRaisesRegex(ValueError, 'differs', self.get)
        self.package(b'self fixture'); self.elf.unlink()
        self.assertRaises(FileNotFoundError, self.get)
    def test_dirty_tree_contents(self):
        clean = self.get()['source']
        (self.repo / 'source.cpp').write_text('change one\n')
        a = self.get()['source']; self.assertTrue(a['dirty']); self.assertNotEqual(a, clean)
        (self.repo / 'source.cpp').write_text('change two\n')
        self.assertNotEqual(a['worktree_sha256'], self.get()['source']['worktree_sha256'])
        p = self.repo / 'new.cpp'; p.write_text('one')
        a = self.get()['source']; p.write_text('two')
        self.assertNotEqual(a['worktree_sha256'], self.get()['source']['worktree_sha256'])
    def test_capture_log_cli(self):
        log = self.root / 'capture.log'
        log.write_text('[AURORA-VITA][FRAME] frame=1 total_us=10 draws=7\n[AURORA-VITA][FRAME] frame=2 total_us=20 draws=8\n')
        command = [sys.executable, str(Path(manifest.__file__)), '--repo', str(self.repo),
                   '--build', str(self.build), '--elf', str(self.elf), '--self', str(self.self_path),
                   '--vpk', str(self.vpk), '--capture-log', str(log)]
        result = subprocess.run(command, text=True, capture_output=True, check=True)
        value = json.loads(result.stdout)
        self.assertEqual(value['frame_samples']['sample_count'], 2)
        self.assertTrue(value['frame_samples']['consecutive_samples'])
        self.assertEqual(value['capture_log']['sha256'], manifest.sha256(log.read_bytes()))
        log.write_text('frames=120 frame_us=10\n')
        result = subprocess.run(command, text=True, capture_output=True)
        self.assertEqual(result.returncode, 2)
        self.assertIn('no FRAME samples', result.stderr)
        log.unlink()
        result = subprocess.run(command, text=True, capture_output=True)
        self.assertEqual(result.returncode, 2)
        self.assertIn('manifest error:', result.stderr)

    def test_installed_identity(self):
        p = self.root / 'capture.json'
        p.write_text(json.dumps({'title': 'fixture', 'installed_eboot_sha256': manifest.sha256(b'self fixture')}))
        result = self.get(capture=p); self.assertTrue(result['installed_hash_verified'])
        self.assertEqual(result['capture']['scene'], 'unknown')
        p.write_text(json.dumps({'installed_eboot_sha256': '0'*64}))
        self.assertRaisesRegex(ValueError, 'does not match', self.get, capture=p)
        p.write_text('[]'); self.assertRaisesRegex(ValueError, 'JSON object', self.get, capture=p)


if __name__ == '__main__':
    unittest.main()
