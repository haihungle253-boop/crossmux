add_executable(AiCompanionTest
  SseDecoderTest.cpp
  AiChatParserTest.cpp
  PromptBuilderTest.cpp
  StreamPipelineTest.cpp
  AiChatClientTest.cpp
  CompanionDataTest.cpp
  CompanionConfigTest.cpp
  ${REPO_ROOT}/lib/AiCompanion/AiChatClient.cpp
  ${REPO_ROOT}/lib/AiCompanion/PersonaStore.cpp
  ${REPO_ROOT}/lib/AiCompanion/QuestionSet.cpp
  ${REPO_ROOT}/lib/AiCompanion/ConversationStore.cpp
  ${REPO_ROOT}/lib/AiCompanion/CompanionConfig.cpp
  ${REPO_ROOT}/lib/AiCompanion/SseDecoder.cpp
  ${REPO_ROOT}/lib/AiCompanion/AiChatParser.cpp
  ${REPO_ROOT}/lib/AiCompanion/PromptBuilder.cpp
  ${REPO_ROOT}/lib/JsonParser/StreamingJsonParser.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)

target_include_directories(AiCompanionTest PRIVATE
  ${REPO_ROOT}/lib/AiCompanion
  ${REPO_ROOT}/lib/JsonParser
  ${REPO_ROOT}/lib/Utf8
)

target_link_libraries(AiCompanionTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

target_compile_definitions(AiCompanionTest PRIVATE
  COMPANION_ASSET_DIR="${REPO_ROOT}/assets/companion"
)

gtest_discover_tests(AiCompanionTest)
