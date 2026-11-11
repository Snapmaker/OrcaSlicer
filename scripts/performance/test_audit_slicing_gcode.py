"""Deterministic fixtures for the regression auditor, not printer validation."""
from pathlib import Path
import tempfile
import unittest

from audit_slicing_gcode import compare, read_gcode


class GCodeAuditTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.prefix = 'G90\nM83\nG1 X0 Y0 Z0.2\n;TYPE:Outer wall\n;HEIGHT:0.2\n;WIDTH:0.4\n'

    def read(self, name, commands):
        path = self.root / name
        path.write_text(self.prefix + commands, encoding='utf-8')
        return read_gcode(path)

    def test_relative_and_absolute_e_with_reset(self):
        a = self.read('a', 'G1 X10 E1 F1200\n')
        b = self.read('b', 'M82\nG92 E4\nG1 X10 E5 F1200\n')
        self.assertEqual(compare(a,b)['changed_groups'], 0)

    def test_direction_and_segment_order(self):
        a = self.read('a', 'G1 X10 E1 F1200\nG1 X20 E2\n')
        b = self.read('b', 'G1 X20\nG1 X10 E2 F1200\nG1 X0 E1\n')
        self.assertEqual(compare(a,b)['changed_groups'], 0)

    def test_wrong_filament_is_not_geometric_equivalence(self):
        a = self.read('a', 'G1 X10 E1 F1200\n')
        b = self.read('b', 'T1\nG1 X10 E1 F1200\n')
        result = compare(a,b,spatial=True)
        self.assertEqual(result['missing_length_mm'], 10)
        self.assertEqual(result['added_length_mm'], 10)

    def test_flow_change_is_detected(self):
        a = self.read('a', 'G1 X10 E1 F1200\n')
        b = self.read('b', 'G1 X10 E2 F1200\n')
        self.assertEqual(compare(a,b)['sum_absolute_group_e_delta_mm'], 1)

    def test_width_change_retained_but_not_reported_as_missing_geometry(self):
        a = self.read('a', 'G1 X10 E1 F1200\n')
        b = self.read('b', ';WIDTH:0.400002\nG1 X10 E1 F1200\n')
        result = compare(a,b,spatial=True)
        self.assertEqual(result['changed_groups'], 2)
        self.assertEqual(result['missing_length_mm'], 0)
        self.assertEqual(result['added_length_mm'], 0)

    def test_arc_preserves_extrusion(self):
        summary, _ = self.read('arc', 'G1 X1\nG3 X1 Y0 I-1 J0 E2 F1200\n')
        self.assertEqual(summary['arc_commands'], 1)
        self.assertAlmostEqual(summary['deposited_e_mm_by_tool'][0], 2)

    def test_unsupported_units_fail(self):
        with self.assertRaises(ValueError): self.read('inch', 'G20\nG1 X10 E1\n')


if __name__ == '__main__':
    unittest.main()
