#ifndef LISA_RESOURCES_H
#define LISA_RESOURCES_H
struct lisa_resource { const char *path; unsigned long size; const char *hash; };
static struct lisa_resource lisa_resources[]={
{"resources/badpage.lotus",1024UL,"bef71e8ebf35465cd4efcc5a8fa9f791c3ac40acb4657238c470f7b32a2ac8c5"},
{"resources/dmalerts.phr",47012UL,"924962a332ca0e251a4eb57c2d812fcc18ddc925fec8ae7bdf2c87901c6dfa10"},
{"resources/font.heur",1536UL,"e8159d9a821124c3faeb2eaac45a9c79e16a25a9e831d1f9966cf8ed2642008d"},
{"resources/font.lib",264070UL,"ee25c73928bcc56561b3457e1512f28fbcf57495824f29e3ca6da2f6fb332a6e"},
{"resources/pr.phr",2459UL,"61d7b165d12610eaff718e0d75c0f70de9b590edef596fae49bae60eecacda8a"},
{"resources/t101.icon",814UL,"4ae56d2675732e5975da1a45f75ef7554bdcd92b95d3a94d976da4cb8e467561"},
{"resources/t140.icon",814UL,"113f776f4f42ad7c97b387c7d8d42e56c645c4cf31930df788f817c08cc1ad7f"},
{"resources/t271828.icon",814UL,"296641f6d697f9417cfcf96739435553e528e6ca662f2baae7e84818b6cc60c2"},
};
#endif
