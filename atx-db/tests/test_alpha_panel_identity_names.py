"""Name stemming used by the alpha panel's FINRA name-match identity tier."""

from atx_db.alpha_panel.identity_names import stem_finra, stem_sec


def test_finra_names_cut_at_security_class_and_truncation() -> None:
    assert stem_finra("Paramount Global Class B Commo") == "paramount global"
    assert stem_finra("Confluent, Inc. Class A Common") == "confluent"
    assert stem_finra("PARAMOUNT GROUP, INC.") == "paramount"
    assert stem_finra("Plug Power, Inc. Common Stock") == "plug power"
    assert stem_finra("AT&T Inc.") == "at and t"


def test_sec_conformed_names_drop_corporate_form_and_state() -> None:
    assert stem_sec("PARAMOUNT GLOBAL") == "paramount global"
    assert stem_sec("CONFLUENT, INC.") == "confluent"
    assert stem_sec("MOODYS CORP /DE/") == "moodys"
    assert stem_sec("AT&T INC.") == "at and t"


def test_trust_names_are_not_cut() -> None:
    assert stem_finra("Agree Realty Trust Common Stock") == "agree realty trust"
    assert stem_sec("AGREE REALTY TRUST") == "agree realty trust"
