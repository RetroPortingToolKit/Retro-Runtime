"""Check crash-prefix recovery without retail data or external packages."""
import importlib.util
import tempfile
import unittest
from pathlib import Path

spec = importlib.util.spec_from_file_location('route_recover', Path(__file__).resolve().parents[1] / 'scripts' / 'route_recover.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class RecoveryTest(unittest.TestCase):
    def test_complete_prefix_and_preserved_source(self):
        import hashlib
        with tempfile.TemporaryDirectory() as folder:
            source, output = Path(folder) / 'movie.partial', Path(folder) / 'recovered.route'
            prefix = (b'RCORE_ROUTE 1\nI 726f6d 74657374\nB 0 0 0 0 0 - - - -\n'
                      b'F 1 0 0 1 1 2d 2d 2d -\nP 0 1 0 0 0 0 0 0 0\nT 7\n')
            data = prefix + b'F 2 0 0 1 1 2d 2d 2d -\nP 0 1 0 0 0'
            source.write_bytes(data)
            self.assertEqual(module.recover(source, output), 1)
            self.assertEqual(output.read_bytes(), prefix + f'END 1 {hashlib.sha256(prefix).hexdigest()}\n'.encode())
            self.assertEqual(source.read_bytes(), data)
            with self.assertRaises(FileExistsError):
                module.recover(source, output)

    def test_anchor_prefix_is_preserved(self):
        with tempfile.TemporaryDirectory() as folder:
            source, output = Path(folder) / 'movie.partial', Path(folder) / 'recovered.route'
            prefix = (b'RCORE_ROUTE 1\nA 2d 46414b45\nB 0 0 0 0 0 - - - -\n'
                      b'F 1 0 0 0 0 2d 2d 2d -\n')
            source.write_bytes(prefix)
            self.assertEqual(module.recover(source, output), 1)
            self.assertTrue(output.read_bytes().startswith(prefix))
            self.assertEqual(source.read_bytes(), prefix)

    def test_invalid_and_empty_prefixes_refuse(self):
        with tempfile.TemporaryDirectory() as folder:
            source, output = Path(folder) / 'movie.partial', Path(folder) / 'recovered.route'
            for data in (b'bad\n', b'RCORE_ROUTE 1\n', b'RCORE_ROUTE 1\nEND 0 bad\n',
                         b'RCORE_ROUTE 1\nB 0 0 0 0 0 - - - -\nF 3 0 0 0 0 - - - -\n'):
                source.write_bytes(data)
                with self.assertRaises(ValueError):
                    module.recover(source, output)
                self.assertFalse(output.exists())


if __name__ == '__main__':
    unittest.main()
