import unittest

from tools.rom_diag_record_parity import compare_records


class RomDiagRecordParityTests(unittest.TestCase):
    def test_matches_same_inferred_identity(self):
        left={"cid":"A","candidates":[{"address":"0x100","proposed_name":"bank1_rec4"}]}
        right={"cid":"B","candidates":[{"address":"0x200","proposed_name":"bank1_rec4"}]}
        parity={"unique_identical_body_mappings":[{
            "left_address":"0x100","right_address":"0x200","sha256":"abc","size":48}]}
        result=compare_records(left,right,parity)
        self.assertEqual(result["stable_candidate_count"],1)
        self.assertEqual(result["disagreement_count"],0)

    def test_reports_identity_disagreement(self):
        left={"cid":"A","candidates":[{"address":"0x100","proposed_name":"bank1_rec4"}]}
        right={"cid":"B","candidates":[{"address":"0x200","proposed_name":"bank2_rec4"}]}
        parity={"unique_identical_body_mappings":[{
            "left_address":"0x100","right_address":"0x200","sha256":"abc","size":48}]}
        self.assertEqual(compare_records(left,right,parity)["disagreement_count"],1)


if __name__ == "__main__":
    unittest.main()
