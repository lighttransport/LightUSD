#!/usr/bin/env python3
"""Regression checks for exact Lean declaration lookup and required binaries."""
import importlib.util
from pathlib import Path
import subprocess
import tempfile
import unittest

SCRIPT = Path(__file__).with_name('check-lean-reflection.py')
spec = importlib.util.spec_from_file_location('reflection', SCRIPT)
reflection = importlib.util.module_from_spec(spec)
spec.loader.exec_module(reflection)


class ReflectionTests(unittest.TestCase):
    def test_exact_namespace_and_mutual_scope(self):
        with tempfile.TemporaryDirectory() as directory:
            Path(directory, 'MisleadingModule.lean').write_text('''namespace Actual
mutual
def first := 1
def second := 2
end
section Example
def third := 3
end Example
inductive Fourth where
  | mk
end Actual
''')
            declarations = reflection.lean_declarations(directory)
        self.assertEqual(set(declarations),
                         {'Actual.first', 'Actual.second', 'Actual.third', 'Actual.Fourth'})
        self.assertIsNotNone(reflection.resolve('Actual.third', declarations))
        for name in ('Wrong.Actual.third', 'MisleadingModule.third', 'Other.third'):
            self.assertIsNone(reflection.resolve(name, declarations))

    def test_required_checker_cannot_silently_skip(self):
        result = subprocess.run(['python3', str(SCRIPT), '--require-lusdchecker',
                                 '--lusdchecker', '/nonexistent/lightusd-lusdchecker'],
                                capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('required lusdchecker binary is missing', result.stdout)


if __name__ == '__main__':
    unittest.main()
