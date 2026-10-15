"""Keep feature ownership and repository navigation intact."""

from pathlib import Path
import re
import unittest
from urllib.parse import unquote


ROOT = Path(__file__).resolve().parents[2]


class SourceLayoutTests(unittest.TestCase):
    def test_shared_sources_have_component_owners(self):
        for name in ("scripts", "tests", "test_support", "cmake"):
            with self.subTest(directory=name):
                self.assertFalse((ROOT / name).exists(), f"Use a module or development component instead of {name}/")
        for name in ("packaging", "testing", "benchmarks"):
            with self.subTest(component=name):
                self.assertTrue((ROOT / "development" / name / "CMakeLists.txt").is_file())

    def test_relative_documentation_links_resolve(self):
        documents = [ROOT / "README.md", ROOT / "AGENTS.md"]
        for name in ("docs", "modules", "development"):
            documents.extend(path for path in (ROOT / name).rglob("*.md") if "vendor" not in path.parts)
        for document in documents:
            for target in re.findall(r"\]\(([^)]+)\)", document.read_text(encoding="utf-8")):
                if re.match(r"[a-zA-Z][a-zA-Z0-9+.-]*:", target) or target.startswith("#"):
                    continue
                path = unquote(target.partition("#")[0])
                with self.subTest(document=document.relative_to(ROOT), target=target):
                    self.assertTrue((document.parent / path).exists(), "Missing local documentation target")


if __name__ == "__main__":
    unittest.main()
