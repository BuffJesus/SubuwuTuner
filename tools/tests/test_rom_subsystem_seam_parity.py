import unittest

from tools.rom_subsystem_seam_parity import compare_seams


class RomSubsystemSeamParityTests(unittest.TestCase):
    def test_matches_same_subsystem_through_exact_body_mapping(self):
        left={"cid":"A","candidates":[
            {"address":"0x100","subsystem":"diagnostics","confidence":"multi_anchor"}]}
        right={"cid":"B","candidates":[
            {"address":"0x240","subsystem":"diagnostics","confidence":"single_anchor"}]}
        parity={"unique_identical_body_mappings":[{
            "left_address":"0x100","right_address":"0x240","size":16,"sha256":"abc"}]}
        result=compare_seams(left,right,parity)
        self.assertEqual(result["stable_association_count"],1)
        self.assertEqual(result["stable_counts_by_subsystem"]["diagnostics"],1)
        self.assertEqual(result["subsystem_disagreement_count"],0)

    def test_reports_different_anchor_classifications(self):
        left={"cid":"A","candidates":[
            {"address":"0x100","subsystem":"diagnostics","confidence":"single_anchor"}]}
        right={"cid":"B","candidates":[
            {"address":"0x240","subsystem":"runtime_io","confidence":"single_anchor"}]}
        parity={"unique_identical_body_mappings":[{
            "left_address":"0x100","right_address":"0x240","size":16,"sha256":"abc"}]}
        result=compare_seams(left,right,parity)
        self.assertEqual(result["stable_association_count"],0)
        self.assertEqual(result["subsystem_disagreement_count"],1)


if __name__ == "__main__":
    unittest.main()
