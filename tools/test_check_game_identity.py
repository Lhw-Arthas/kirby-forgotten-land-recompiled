"""tools/check_game_identity.py on a copy of the repository's files with one value changed at a time:
python tools/test_check_game_identity.py"""
import os
import shutil
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import check_game_identity as cgi

REPO = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
FILES = ['game/game.h', 'game/game.mk', 'runtime/game_addresses/versions.h', 'runtime/settings.c',
         'launcher/src-tauri/src/main.rs', 'launcher/src-tauri/tauri.conf.json', 'launcher/src-tauri/Cargo.toml',
         'launcher/src-tauri/Cargo.lock', 'launcher/package.json', 'launcher/package-lock.json', 'recomplauncher.json',
         'README.md', 'docs/release-README.txt', 'CHANGELOG.md']


class Identity(unittest.TestCase):
    def setUp(self):
        self.root = tempfile.mkdtemp()
        for f in FILES:
            os.makedirs(os.path.dirname(os.path.join(self.root, f)), exist_ok=True)
            shutil.copy(os.path.join(REPO, f), os.path.join(self.root, f))
        self.edit('CHANGELOG.md', '## ', '## 0.3.0\n\n## ')  # (the newest entry names the version being built)

    def tearDown(self):
        shutil.rmtree(self.root)

    def edit(self, path, old, new):
        p = os.path.join(self.root, path)
        with open(p, encoding='utf-8', newline='') as f:
            s = f.read()
        self.assertIn(old, s, f'{path} has no {old!r}')
        with open(p, 'w', encoding='utf-8', newline='') as f:
            f.write(s.replace(old, new, 1))

    def errors(self):
        return '\n'.join(cgi.check(self.root))

    def test_the_repository_agrees_with_itself(self):
        self.assertEqual(self.errors(), '')

    def test_a_new_version_must_reach_the_launcher_and_the_changelog(self):
        self.edit('game/game.mk', 'GAME_VERSION := ', 'GAME_VERSION := 9.9.9 #')
        e = self.errors()
        for where in ('tauri.conf.json version', 'launcher/package.json', 'Cargo.toml', 'Cargo.lock', 'CHANGELOG.md'):
            self.assertIn(where, e)

    def test_another_program_id_must_reach_the_launcher_and_the_readmes(self):
        self.edit('game/game.h', '0x01004D300C5AE000ull', '0x0100123400005000ull')
        e = self.errors()
        for where in ('PROGRAM_ID', 'UPDATE_ID', 'README.md does not name', 'docs/release-README.txt does not name'):
            self.assertIn(where, e)

    def test_another_exe_name_must_reach_the_manifest_and_the_launcher(self):
        self.edit('game/game.mk', 'EXE_NAME     := ForgottenLandRecomp', 'EXE_NAME     := Other')
        e = self.errors()
        for where in ('PACKAGED_EXE', 'prepare program', 'start program', 'does not name Other.exe'):
            self.assertIn(where, e)

    def test_the_manifest_must_name_a_supported_build_id(self):
        self.edit('recomplauncher.json', 'd9ba7db72ffafecd07f5f5535dec05734e59bce1', '0' * 40)
        self.assertIn('nup_build_id', self.errors())

    def test_a_setting_the_runtime_does_not_read_is_caught(self):
        self.edit('recomplauncher.json', '"resolution": "match"', '"render_height": "0"')
        self.assertIn('render_height', self.errors())

    def test_a_staged_release_needs_its_files(self):
        dist = os.path.join(self.root, 'stage')
        os.makedirs(dist)
        e = '\n'.join(cgi.check_dist(self.root, dist))
        for name in ('ForgottenLandRecomp.exe', 'ForgottenLandLauncher.exe', 'README.txt', 'recomplauncher.json'):
            self.assertIn(name, e)
        for name in ('ForgottenLandRecomp.exe', 'ForgottenLandLauncher.exe', 'lasrecomp.dll.a', 'LICENSE.txt'):
            open(os.path.join(dist, name), 'w').close()
        shutil.copy(os.path.join(self.root, 'recomplauncher.json'), dist)
        shutil.copy(os.path.join(self.root, 'docs/release-README.txt'), os.path.join(dist, 'README.txt'))
        self.assertEqual(cgi.check_dist(self.root, dist), [])


if __name__ == '__main__':
    unittest.main()
