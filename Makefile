# make verify: CI와 같은 명령입니다. 빌드, 단위 테스트, 기능 테스트, 코드 모양 검사를
# 차례로 실행합니다. PR을 올리기 전에 로컬에서 먼저 실행합니다.
#
# 소스 배치 규칙
#   src/*.c          여러 실행 파일과 테스트가 함께 링크하는 모듈 (main 없음)
#   src/main/<이름>.c main()이 있는 파일. 파일마다 build/<이름> 실행 파일 하나가 됩니다
#   tests/<모듈>_test.c          단위 테스트. 파일마다 build/test/<모듈>_test 하나가 됩니다
#   tests/functional/<이름>_test.py  기능 테스트. 첫 인자로 build 폴더 경로를 받습니다
# 파일을 추가하면 Makefile을 고치지 않아도 빌드 대상에 들어갑니다.
# src/main/의 파일 이름은 build/ 아래 폴더 이름(obj, test, release, tsan)과 겹치면 안 됩니다.

CC       ?= cc
CPPFLAGS := -Isrc -D_POSIX_C_SOURCE=200809L
# macOS 헤더는 _POSIX_C_SOURCE만 주면 일부 BSD 선언을 숨깁니다. acutest가 그 선언을 씁니다.
ifeq ($(shell uname -s),Darwin)
CPPFLAGS += -D_DARWIN_C_SOURCE
endif

# 개발 빌드입니다. 경고를 오류로 보고, ASan과 UBSan으로 메모리 오류와 정의되지 않은
# 동작을 실행 중에 바로 멈춰 세웁니다. Linux에서는 ASan이 종료 시 메모리 누수도 검사합니다.
# -MMD -MP는 .o마다 읽은 헤더 목록(.d)을 남겨, 헤더만 고쳐도 다시 빌드되게 합니다.
CFLAGS   := -std=c11 -Wall -Wextra -Werror -g -fno-omit-frame-pointer \
            -fsanitize=address,undefined -fno-sanitize-recover=all \
            -MMD -MP -pthread
LDFLAGS  := -fsanitize=address,undefined -pthread

BUILD    := build

LIB_SRCS  := $(wildcard src/*.c)
MAIN_SRCS := $(wildcard src/main/*.c)
LIB_OBJS  := $(LIB_SRCS:src/%.c=$(BUILD)/obj/%.o)
PROGRAMS  := $(MAIN_SRCS:src/main/%.c=$(BUILD)/%)

# tests/acutest.h: https://github.com/mity/acutest (MIT),
# commit 31751b4089c93b46a9fd8a8183a695f772de66de 의 include/acutest.h 그대로입니다.
TEST_SRCS := $(wildcard tests/*_test.c)
TEST_BINS := $(TEST_SRCS:tests/%.c=$(BUILD)/test/%)

FUNCTIONAL_TESTS := $(wildcard tests/functional/*_test.py)

.PHONY: all test functional verify lint format release tsan clean help

all: $(PROGRAMS) $(TEST_BINS)

$(PROGRAMS): $(BUILD)/%: src/main/%.c $(LIB_OBJS) | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(LIB_OBJS) $(LDFLAGS) -o $@

$(BUILD)/obj/%.o: src/%.c | $(BUILD)/obj
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD)/test/%_test: tests/%_test.c tests/acutest.h $(LIB_OBJS) | $(BUILD)/test
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(LIB_OBJS) $(LDFLAGS) -o $@

$(BUILD) $(BUILD)/obj $(BUILD)/test:
	mkdir -p $@

-include $(wildcard $(BUILD)/*.d $(BUILD)/obj/*.d $(BUILD)/test/*.d)

test: $(TEST_BINS)
	@if [ -z "$(TEST_BINS)" ]; then echo "test: 단위 테스트 파일이 아직 없습니다"; fi
	@set -e; for t in $(TEST_BINS); do echo "== $$t"; ./$$t; done

# 기능 테스트는 실행 파일을 띄우고 바깥에서 요청을 보냅니다. -B는 .pyc를 만들지 않게 합니다.
functional: $(PROGRAMS)
	@if [ -z "$(FUNCTIONAL_TESTS)" ]; then echo "functional: 기능 테스트 파일이 아직 없습니다"; fi
	@set -e; for t in $(FUNCTIONAL_TESTS); do echo "== $$t"; python3 -B $$t $(BUILD); done

verify: all test functional lint

# 코드 모양 검사입니다(CONVENTION.md의 "코드"). 모양은 .clang-format이 정하고,
# .clang-format-ignore에 적은 파일은 clang-format이 스스로 건너뜁니다.
# git이 아는 파일만 봅니다. 새 파일은 git add 뒤에 검사됩니다.
# lint는 위반이나 설정 오류가 있으면 실패로 끝나고(--Werror), format은 파일을 고칩니다.
FORMAT_FILES = $(shell git ls-files -- 'src/*.c' 'src/*.h' 'tests/*.c' 'tests/*.h')

lint:
	@if [ -z "$(FORMAT_FILES)" ]; then echo "lint: 검사할 C 파일이 아직 없습니다"; \
	else clang-format --dry-run --Werror $(FORMAT_FILES) && echo "lint: 위반 없음"; fi

format:
	@if [ -z "$(FORMAT_FILES)" ]; then echo "format: 고칠 C 파일이 아직 없습니다"; \
	else clang-format -i $(FORMAT_FILES) && echo "format: 완료 (바뀐 줄은 git diff로 확인)"; fi

# make release: 부하 테스트(ab, wrk, hey)용 빌드입니다. sanitizer는 서버를 몇 배 느리게
# 만들어 처리량 숫자에 의미가 없으므로, sanitizer를 끄고 -O2로 build/release/에 따로
# 만듭니다. make verify와 CI에는 넣지 않습니다.
RELEASE_BUILD    := $(BUILD)/release
RELEASE_CFLAGS   := -std=c11 -Wall -Wextra -Werror -O2 -MMD -MP -pthread
RELEASE_LIB_OBJS := $(LIB_SRCS:src/%.c=$(RELEASE_BUILD)/obj/%.o)
RELEASE_PROGRAMS := $(MAIN_SRCS:src/main/%.c=$(RELEASE_BUILD)/%)

release: $(RELEASE_PROGRAMS)
	@if [ -z "$(RELEASE_PROGRAMS)" ]; then echo "release: src/main/에 실행 파일 소스가 아직 없습니다"; fi

$(RELEASE_PROGRAMS): $(RELEASE_BUILD)/%: src/main/%.c $(RELEASE_LIB_OBJS) | $(RELEASE_BUILD)
	$(CC) $(CPPFLAGS) $(RELEASE_CFLAGS) $< $(RELEASE_LIB_OBJS) -pthread -o $@

$(RELEASE_BUILD)/obj/%.o: src/%.c | $(RELEASE_BUILD)/obj
	$(CC) $(CPPFLAGS) $(RELEASE_CFLAGS) -c $< -o $@

$(RELEASE_BUILD) $(RELEASE_BUILD)/obj:
	mkdir -p $@

-include $(wildcard $(RELEASE_BUILD)/*.d $(RELEASE_BUILD)/obj/*.d)

# make tsan: 단위 테스트를 ThreadSanitizer로 빌드해 돌립니다. 스레드를 쓰는 코드의
# 데이터 경합을 찾습니다. TSan은 ASan과 함께 켤 수 없어 build/tsan/에 따로 빌드합니다.
# 동시성 모델을 정하기 전이라 make verify와 CI에는 아직 넣지 않습니다.
TSAN_BUILD    := $(BUILD)/tsan
TSAN_CFLAGS   := -std=c11 -Wall -Wextra -Werror -g -O1 -fno-omit-frame-pointer \
                 -fsanitize=thread -MMD -MP -pthread
TSAN_LIB_OBJS := $(LIB_SRCS:src/%.c=$(TSAN_BUILD)/obj/%.o)
TSAN_TESTS    := $(TEST_SRCS:tests/%.c=$(TSAN_BUILD)/%)

# acutest는 테스트마다 자식 프로세스를 만들고 그 종료 코드를 결과로 읽습니다. TSan의
# 기본 종료 코드 66은 실패로 세지 않으므로, 경고가 실패(1)로 읽히게 바꿉니다.
tsan: $(TSAN_TESTS)
	@if [ -z "$(TSAN_TESTS)" ]; then echo "tsan: 단위 테스트 파일이 아직 없습니다"; fi
	@set -e; for t in $(TSAN_TESTS); do echo "== $$t (tsan)"; TSAN_OPTIONS=exitcode=1 ./$$t; done

$(TSAN_BUILD)/%_test: tests/%_test.c tests/acutest.h $(TSAN_LIB_OBJS) | $(TSAN_BUILD)
	$(CC) $(CPPFLAGS) $(TSAN_CFLAGS) $< $(TSAN_LIB_OBJS) -fsanitize=thread -pthread -o $@

$(TSAN_BUILD)/obj/%.o: src/%.c | $(TSAN_BUILD)/obj
	$(CC) $(CPPFLAGS) $(TSAN_CFLAGS) -c $< -o $@

$(TSAN_BUILD) $(TSAN_BUILD)/obj:
	mkdir -p $@

-include $(wildcard $(TSAN_BUILD)/*.d $(TSAN_BUILD)/obj/*.d)

clean:
	rm -rf $(BUILD)

help:
	@echo "make            개발 빌드 (ASan + UBSan): build/<이름>, build/test/<모듈>_test"
	@echo "make test       단위 테스트 실행"
	@echo "make functional 기능 테스트 실행 (실제 서버를 띄워 요청)"
	@echo "make lint       코드 모양 검사 (위반이나 설정 오류 시 실패)"
	@echo "make format     코드 모양 자동 수정"
	@echo "make verify     CI와 같은 전체 검증: all + test + functional + lint"
	@echo "make release    부하 테스트용 -O2 빌드: build/release/<이름>"
	@echo "make tsan       단위 테스트를 ThreadSanitizer로 실행"
	@echo "make clean      build/ 삭제"
