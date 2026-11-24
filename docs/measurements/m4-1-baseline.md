# M4-1 연결당 스레드: 부하 측정 비교 기준

M8 부하 측정에서 스레드 풀(M4-2 이후)과 비교할 기준 버전입니다 (final.md 2.1, 8.7).

| 항목 | 값 |
| --- | --- |
| 커밋 | `0c89b54a209f0d72686a14ffda6aeb1d0b926d78` |
| 브랜치 | `phase2/bloro` |
| 동시성 모델 | 연결마다 `pthread_create` + `pthread_detach` |
| 실행 인자 | `<port>`만 받음 (`--threads` 등 없음) |

## 빌드 방법

작업 폴더를 건드리지 않도록 별도 worktree에서 빌드합니다.

```bash
git worktree add ../tiny-m4-1 0c89b54a209f0d72686a14ffda6aeb1d0b926d78
cd ../tiny-m4-1
make release   # build/release/tiny (sanitizer 없음, -O2)
```
