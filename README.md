# Week13_webserver

C와 POSIX 소켓 API로 만드는 HTTP 웹 서버와 Proxy 서버입니다. 크래프톤 정글 13주차 과제입니다.

## 시작하기

macOS 또는 Linux에서 개발합니다. Windows에서는 LMS가 허용한 WSL2+Ubuntu, Docker, 클라우드 중 하나를 쓰며, 무엇을 쓸지는 팀이 정합니다. 필요한 도구와 설치 명령은 [CONVENTION.md의 "개발 환경"](CONVENTION.md#개발-환경)에 있습니다.

```bash
make verify   # 빌드, 단위 테스트, 기능 테스트, 코드 모양 검사 (CI와 같은 명령)
make help     # 다른 명령 목록
```

## 문서

| 문서 | 내용 |
| --- | --- |
| [CONVENTION.md](CONVENTION.md) | 팀 규칙 초안, 개발 환경, 빌드 명령, 코드 규칙, 테스트 규칙 |
| [docs/prd/](docs/prd/README.md) | 개인 PRD 초안, 최종 PRD |
| [docs/measurements/](docs/measurements/) | 부하 테스트 등 측정 기록 |
