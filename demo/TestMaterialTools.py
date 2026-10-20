"""Device-free integration checks for the real MaterialTool executable.

Run: python TestMaterialTools.py <AYRenderer_MaterialTool.exe>
No source assets are changed; each case owns a temporary copied library.
"""
from pathlib import Path
import copy
import json
import math
import shutil
import subprocess
import sys
import tempfile
import unittest
import GenerateMaterialLibrary as library

TOOL = Path(sys.argv.pop(1)).resolve()
SOURCE = Path(__file__).resolve().parent / 'MaterialWorkshop' / 'Assets' / 'materials'


class MaterialToolContracts(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='ay-material-tools-')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.base = json.loads((SOURCE / 'library.json').read_text(encoding='utf-8'))['materials'][0]
        self.recipe = dict(schemaVersion=1, materials=[copy.deepcopy(self.base)])
        for reference in self.base['textures'].values():
            destination = self.root / reference
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(SOURCE / reference, destination)

    def build(self):
        (self.root / 'library.json').write_text(json.dumps(self.recipe), encoding='utf-8')
        return subprocess.run([str(TOOL), 'build', str(self.root)], capture_output=True, timeout=20)

    def assert_refused_without_writes(self):
        shader = self.root / 'pbr.phoskia'
        material = self.root / 'metal.mat'
        shader.write_bytes(b'existing shader')
        material.write_bytes(b'existing material')
        result = self.build()
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertEqual(shader.read_bytes(), b'existing shader')
        self.assertEqual(material.read_bytes(), b'existing material')

    def test_native_material_and_shader_are_written_and_inspectable(self):
        result = self.build()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue((self.root / 'pbr.phoskia').is_file())
        inspect = subprocess.run([str(TOOL), 'inspect', str(self.root / 'metal.mat')],
                                 capture_output=True, timeout=20)
        self.assertEqual(inspect.returncode, 0, inspect.stderr)
        self.assertIn(b'Satin Steel', inspect.stdout)
        self.assertIn(b'pbr.phoskia', inspect.stdout)
        self.assertEqual((self.root / 'metal.mat').read_bytes()[:4], b'AYMT')

    def test_invalid_numeric_enum_and_vector_values_are_rejected_before_publish(self):
        invalid = [('roughness', 1.01), ('metallic', -1), ('normalStrength', 5),
                   ('roughnessBias', -1.01), ('uvTransform', [0, 1, 0, 0]),
                   ('uvTransform', [1, 1, 0]), ('alphaMode', 1.5),
                   ('opacitySource', .5), ('opacitySource', 3)]
        for field, value in invalid:
            with self.subTest(field=field, value=value):
                self.recipe['materials'] = [copy.deepcopy(self.base)]
                self.recipe['materials'][0][field] = value
                self.assert_refused_without_writes()

    def test_all_entries_are_prevalidated_before_any_material_is_published(self):
        invalid = copy.deepcopy(self.base)
        invalid['id'] = 'second'
        invalid['normalStrength'] = -1
        self.recipe['materials'].append(invalid)
        self.assert_refused_without_writes()
        self.assertFalse((self.root / 'second.mat').exists())

    def test_duplicate_ids_unknown_fields_and_slots_are_rejected(self):
        self.recipe['materials'].append(copy.deepcopy(self.base))
        self.assert_refused_without_writes()
        self.recipe['materials'] = [copy.deepcopy(self.base)]
        self.recipe['materials'][0]['unrecognized'] = True
        self.assert_refused_without_writes()
        self.recipe['materials'] = [copy.deepcopy(self.base)]
        self.recipe['materials'][0]['textures']['unknownTexture'] = 'textures/metal_d.png'
        self.assert_refused_without_writes()

    def test_missing_and_escaped_dependencies_are_rejected(self):
        self.recipe['materials'][0]['textures']['normalTexture'] = '../outside.png'
        self.assert_refused_without_writes()
        self.recipe['materials'][0]['textures']['normalTexture'] = 'textures/missing.png'
        self.assert_refused_without_writes()
        self.recipe['materials'][0]['textures']['normalTexture'] = self.base['textures']['normalTexture'] + '\0hidden'
        self.assert_refused_without_writes()

    def test_oversized_recipe_and_fractional_schema_are_rejected(self):
        self.recipe['materials'] = [dict(self.base, id=f'm{i}') for i in range(129)]
        self.assert_refused_without_writes()
        self.recipe = dict(schemaVersion=1.0, materials=[copy.deepcopy(self.base)])
        self.assert_refused_without_writes()
        self.recipe = dict(schemaVersion=1, materials=[dict(self.base, name='bad\0name')])
        self.assert_refused_without_writes()


class ProceduralSurfaceContracts(unittest.TestCase):
    def test_material_fields_remain_finite_and_repeat_across_both_uv_seams(self):
        def values(sample):
            color, height, roughness, metallic, ao, emission = sample
            return (*color, height, roughness, metallic, ao, *emission)
        for kind, _ in library.MATERIALS:
            for variant in ['base', 'clean', 'wet', 'worn']:
                for u, v in [(0., 0.), (.125, .375), (.5, .875), (.875, .125)]:
                    original = values(library.surface(kind, u, v, variant))
                    self.assertTrue(all(math.isfinite(value) for value in original), (kind, variant))
                    for du, dv in [(1, 0), (0, 1), (-1, 1)]:
                        repeated = values(library.surface(kind, u+du, v+dv, variant))
                        self.assertEqual(original, repeated, (kind, variant, du, dv))

    def test_new_finish_families_keep_their_intended_material_roles(self):
        samples = {kind: [library.surface(kind, x/16, y/16) for y in range(16) for x in range(16)]
                   for kind in ['copper', 'concrete', 'terrazzo', 'leather', 'asphalt', 'brick', 'marble', 'plastic', 'acrylic']}
        copper_metal = [value[3] for value in samples['copper']]
        self.assertGreater(max(copper_metal), .9)
        self.assertLess(min(copper_metal), .2)
        for kind in ['concrete', 'terrazzo', 'leather', 'asphalt', 'brick', 'marble', 'plastic', 'acrylic']:
            self.assertTrue(all(value[3] == 0 for value in samples[kind]), kind)
            self.assertTrue(all(value[5] == (0, 0, 0) for value in samples[kind]), kind)
        self.assertGreater(sum(v[2] for v in samples['brick']), sum(v[2] for v in samples['marble']))
        self.assertGreater(max(v[1] for v in samples['leather']) - min(v[1] for v in samples['leather']), .01)


if __name__ == '__main__':
    unittest.main()
