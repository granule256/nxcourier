"""使用真实工具处理函数、宿主 POSIX 文件系统和最薄 libnx 类型适配。"""
import pathlib, subprocess, tempfile, unittest
ROOT=pathlib.Path(__file__).resolve().parents[1]
class FilesSaves(unittest.TestCase):
 def test_fs_actual_handlers(self):
  with tempfile.TemporaryDirectory() as d:
   d=str(pathlib.Path(d).resolve());p=pathlib.Path(d);(p/'switch.h').write_text('#include "'+str(ROOT/'tests/test_files_saves_switch.hpp')+'"')
   subprocess.run(['c++','-std=c++17','-Wno-deprecated-declarations','-I'+d,'-I'+str(ROOT/'device/platform/include'),str(ROOT/'tests/test_files_saves_fs.cpp'),'-o',d+'/fs'],check=True)
   (p/'sd').mkdir();subprocess.run([d+'/fs',d+'/sd'],check=True)
 def test_save_actual_handlers(self):
  with tempfile.TemporaryDirectory() as d:
   d=str(pathlib.Path(d).resolve());p=pathlib.Path(d);(p/'switch.h').write_text('#include "'+str(ROOT/'tests/test_files_saves_switch.hpp')+'"')
   subprocess.run(['c++','-std=c++17','-Wno-deprecated-declarations','-I'+d,'-I'+str(ROOT/'device/platform/include'),str(ROOT/'tests/test_files_saves_save.cpp'),'-o',d+'/save'],check=True)
   subprocess.run([d+'/save',d],check=True)
if __name__=='__main__':unittest.main()
