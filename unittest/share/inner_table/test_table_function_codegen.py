#!/usr/bin/env python3
"""Check generated system-source capabilities without building or creating a DB."""
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest


class TableFunctionGenerationTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.repo = Path(__file__).resolve().parents[3]
        cls.output = tempfile.TemporaryDirectory(prefix='table-function-codegen-')
        cls.addClassCleanup(cls.output.cleanup)
        cls.share = Path(cls.output.name) / 'share'
        cls.observer = Path(cls.output.name) / 'observer'
        subprocess.run([
            sys.executable,
            str(cls.repo / 'src/share/inner_table/generate_inner_table_schema.py'),
            '--share-output-dir', str(cls.share),
            '--observer-output-dir', str(cls.observer), '--quiet',
        ], check=True, env={**os.environ, 'PYTHONDONTWRITEBYTECODE': '1'})
        cls.misc = (cls.share / 'ob_inner_table_schema_misc.ipp').read_text()
        cls.header = (cls.share / 'ob_inner_table_schema.h').read_text()
        cls.manifest = json.loads((cls.repo / 'docs/implementation/table-function-providers.json').read_text())

    def test_catalog_covers_every_live_provider(self):
        declarations = re.findall(
            r'^  \{(\d+), "([^"]+)", "([^"]+)", .*?, (-?\d+), (-?\d+), system_columns_',
            self.misc, re.MULTILINE)
        actual = {(int(i), n, s, int(low), int(high)) for i, n, s, low, high in declarations}
        expected = {(v['old_id'], v['function'], v['signature'], v['min_arguments'], v['max_arguments'])
                    for v in self.manifest}
        self.assertEqual(actual, expected)
        self.assertEqual(len(declarations), len(actual))
        self.assertEqual(len({row[1] for row in actual}), len(actual))
        # Every live definition has exactly one handwritten/generated factory case.
        factory = (self.repo / 'src/observer/virtual_table/ob_virtual_table_iterator_factory.cpp').read_text()
        ids = dict(re.findall(r'const uint64_t (OB_\w+_TID) = (\d+);',
                              (self.share / 'ob_inner_table_schema_constants.h').read_text()))
        cases = re.findall(r'case (OB_\w+_TID)\s*:', factory + self.misc.split('#ifdef LOCAL_VIRTUAL_TABLE_CREATE_ITER')[1].split('#endif // LOCAL_VIRTUAL_TABLE_CREATE_ITER')[0])
        self.assertEqual({int(ids[name]) for name in cases}, {row[0] for row in actual})
        self.assertEqual(len(cases), len(actual))

    def test_output_columns_preserve_provider_order_and_count(self):
        total = 0
        for provider in self.manifest:
            block = re.search(r'system_columns_%d\[\] = \{(.*?)\n\};' % provider['old_id'],
                              self.misc, re.DOTALL).group(1)
            columns = re.findall(r'make_system_table_column\("([^"]+)".*?OB_APP_MIN_COLUMN_ID \+ (\d+)\)', block)
            self.assertEqual(len(columns), provider['column_count'])
            self.assertEqual([int(i) for _, i in columns], list(range(len(columns))))
            self.assertEqual(len({name.lower() for name, _ in columns}), len(columns))
            total += len(columns)
        self.assertEqual(total, 1664)
        memory = re.search(r'system_columns_11037\[\] = \{(.*?)\n\};', self.misc, re.DOTALL).group(1)
        self.assertEqual(re.findall(r'make_system_table_column\("([^"]+)"', memory),
                         ['ctx_id', 'label', 'ctx_name', 'mod_type', 'mod_id',
                          'mod_name', 'hold', 'used', 'count'])

    def test_functions_have_no_schema_creators_or_bootstrap_count(self):
        self.assertNotIn('virtual_table_schema_creators', self.header)
        self.assertIn('OB_VIRTUAL_TABLE_COUNT = 0;', self.header)
        self.assertIn('OB_SYS_VIEW_COUNT = 139;', self.header)
        sources = '\n'.join(p.read_text() for p in self.share.glob('ob_inner_table_schema.*.cpp'))
        for provider in self.manifest:
            method = provider['old_name'].replace('$', '_').lower().strip('_') + '_schema('
            self.assertNotIn('ObInnerTableSchema::' + method, sources)
        self.assertIn('ObInnerTableSchema::all_table_schema(', sources)

    def test_system_views_do_not_bind_removed_schema_names(self):
        sources = '\n'.join(p.read_text() for p in self.share.glob('ob_inner_table_schema.*.cpp'))
        for provider in self.manifest:
            name = re.escape(provider['old_name'])
            if not provider['old_name'].lower().startswith('__all_virtual_'):
                database = 'mysql' if provider['old_name'] in ('user', 'db') else 'information_schema'
                name = database + r'\.' + name
            else:
                name = r'(?:oceanbase\.)?' + name
            self.assertIsNone(re.search(r'(?:FROM|JOIN|,)\s+' + name + r'(?=\s|[;,])',
                                        sources, re.IGNORECASE), provider['old_name'])
        # Preserve the column view's targeted lookup and the SQL visibility filters.
        self.assertEqual(sources.count('information_columns(D.DATABASE_NAME, T.TABLE_NAME)'), 2)
        self.assertIn("sys_privilege_check('table_acc', D.DATABASE_NAME, T.TABLE_NAME)", sources)

    def test_protected_catalogs_are_not_registered_as_public(self):
        for name in ('mysql_users', 'mysql_database_privileges'):
            descriptor = next(line for line in self.misc.splitlines() if ', "' + name + '",' in line)
            self.assertTrue(descriptor.endswith('"mysql"},'), descriptor)
        memory = next(line for line in self.misc.splitlines() if ', "memory_info",' in line)
        self.assertTrue(memory.endswith('"oceanbase"},'), memory)

    def test_required_locator_inputs_and_safe_lookup_columns(self):
        required = {
            'table_status': (1, 1), 'table_columns': (1, 1),
            'show_tables': (1, 1), 'show_create_table': (1, 1),
            'tx_data': (1, 1), 'mds_node_stat': (1, -1),
            'privilege_grant': (1, -1),
        }
        for name, counts in required.items():
            provider = next(v for v in self.manifest if v['function'] == name)
            self.assertEqual((provider['min_arguments'], provider['max_arguments']), counts)
        # Only the two leading name keys of information_columns accept lookups.
        declarations = re.findall(r'static const uint64_t system_lookup_columns_(\d+)\[\] = \{([^}]+)\};', self.misc)
        information_columns = next(v for v in self.manifest if v['function'] == 'information_columns')
        self.assertEqual(declarations, [(str(information_columns['old_id']),
                                        'OB_APP_MIN_COLUMN_ID + 0, OB_APP_MIN_COLUMN_ID + 1')])

    def test_local_storage_and_cursor_cleanup_survive(self):
        storage = (self.share / 'local_table_schema.h').read_text()
        # Local persistent data is independent of the removed SQL facade schemas.
        self.assertIn('namespace tablet_meta', storage)
        self.assertIn('namespace deadlock_history', storage)
        self.assertIn('namespace admin_job', storage)
        providers = (self.observer / 'ob_all_virtual_local_tables.cpp').read_text()
        self.assertEqual(providers.count('open_cursor(select_request, cursor_)'), 6)
        self.assertEqual(providers.count('cursor_.reset();'), 7)
        self.assertNotIn('ObTableSchema', providers)


if __name__ == '__main__':
    unittest.main()
