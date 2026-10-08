import pytest
from atx_db.alpha_panel import events_sources as ES


class Rate:
    def __init__(self):
        self.labels = []

    def acquire(self, label):
        self.labels.append(label)
        return 1.0

    def record_response(self, *args, **kwargs):
        pass


def test_attempt_budget_persists_retry_redirect_and_resume(tmp_path):
    rate = Rate()
    limiter = ES.AttemptLimiter(tmp_path, 3, 59990, 7, rate)
    for label in ('first', 'retry', 'redirect'):
        limiter.acquire(label)
    assert rate.labels == ['first', 'retry', 'redirect']
    assert len((tmp_path/'attempts.jsonl').read_text().splitlines()) == 3
    resumed = ES.AttemptLimiter(tmp_path, 3, 59990, 7, rate)
    with pytest.raises(ES.BudgetExceeded):
        resumed.acquire('fourth')
    assert len(rate.labels) == 3


def test_allocation_cannot_raise_global_ceiling(tmp_path):
    with pytest.raises(ValueError, match='60000'):
        ES.AttemptLimiter(tmp_path, 4, 59990, 7)


def test_interrupted_rate_acquisition_counts_conservatively(tmp_path):
    class Interrupted(Rate):
        def acquire(self, label):
            raise RuntimeError('stop')
    limiter = ES.AttemptLimiter(tmp_path, 1, 0, inner=Interrupted())
    with pytest.raises(RuntimeError):
        limiter.acquire('first')
    assert ES.AttemptLimiter(tmp_path, 1, 0, inner=Rate()).state['attempts'] == 1


def test_pilot_redirect_attempts_and_cumulative_byte_caps(tmp_path,monkeypatch):
    monkeypatch.setattr(ES,'RAW_DIR',tmp_path)
    original_limiter = ES.AttemptLimiter
    monkeypatch.setattr(ES,'AttemptLimiter',lambda root,allocation,prior,reserved:original_limiter(root,allocation,prior,reserved,Rate()))
    class Store:
        def __init__(self):
            self.records=[]
            self.objects=[]
        def lookup(self,url):
            return None
        def _write_object(self,sha,body):
            self.objects.append((sha,body))
        def _append(self,record,**kwargs):
            self.records.append(record)
    class Response:
        def __init__(self,status,location=None):
            self.status_code=status
            self.headers={'Location':location} if location else {}
            self.body=b'body'
        def __enter__(self):return self
        def __exit__(self,*args):pass
    class Session:
        def __init__(self,limiter):self.limiter=limiter; self.urls=[]
        def get(self,url,**kwargs):
            assert kwargs['allow_redirects'] is False and kwargs['stream'] is True
            self.limiter.acquire(url)
            self.urls.append(url)
            return Response(302,'next.htm') if len(self.urls)==1 else Response(200)
        def close(self):pass
    sessions=[]
    def session(**kwargs):
        assert kwargs['max_attempts']==1
        sessions.append(Session(kwargs['limiter']))
        return sessions[-1]
    monkeypatch.setattr(ES.SH,'sec_session',session)
    monkeypatch.setattr(ES.SH,'read_bounded_response',lambda response,maximum:response.body)
    store=Store()
    urls=['https://www.sec.gov/Archives/edgar/data/1/000000000000000001/ex99.htm']
    result=ES.fetch_fpi_pilot(urls,allocation=3,global_prior=0,store=store,
                            per_response=16,response_cap=2*(16+65536),compressed_cap=2*(16+1024))
    assert result['attempts']==2 and result['bytes']['response_bytes']==8
    assert len(store.objects)==2 and store.records[0].ok
    assert len((tmp_path/'task9_pilot/receipts.jsonl').read_text().splitlines())==2
    # Same frozen selection resumes with no further body/attempt headroom.
    result=ES.fetch_fpi_pilot(urls,allocation=3,global_prior=0,store=Store(),
                            per_response=16,response_cap=2*(16+65536),compressed_cap=2*(16+1024))
    assert result['stop']=='cumulative_byte_reservation_exhausted' and result['attempts']==2
