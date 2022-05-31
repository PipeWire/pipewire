#include <vulkan/vulkan.h>

#include <spa/buffer/buffer.h>
#include <spa/node/node.h>

#define MAX_STREAMS 2
#define MAX_BUFFERS 16
#define WORKGROUP_SIZE 32

struct pixel {
	float r, g, b, a;
};

struct push_constants {
	float time;
	int frame;
	int width;
	int height;
};

struct vulkan_buffer {
	int fd;
	VkImage image;
	VkImageView view;
	VkDeviceMemory memory;
};

struct vulkan_stream {
	enum spa_direction direction;

	uint32_t pending_buffer_id;
	uint32_t current_buffer_id;
	uint32_t busy_buffer_id;
	uint32_t ready_buffer_id;

	struct vulkan_buffer buffers[MAX_BUFFERS];
	uint32_t n_buffers;
};

struct vulkan_state {
	struct spa_log *log;

	struct push_constants constants;

	VkInstance instance;

	VkPhysicalDevice physicalDevice;
	VkDevice device;

	VkPipeline pipeline;
	VkPipelineLayout pipelineLayout;
	const char *shaderName;
	VkShaderModule computeShaderModule;

	VkCommandPool commandPool;
	VkCommandBuffer commandBuffer;

	VkQueue queue;
	uint32_t queueFamilyIndex;
	VkFence fence;
	unsigned int prepared:1;
	unsigned int started:1;

	VkDescriptorPool descriptorPool;
	VkDescriptorSetLayout descriptorSetLayout;

	VkSampler sampler;

	uint32_t n_streams;
	VkDescriptorSet descriptorSet;
	struct vulkan_stream streams[MAX_STREAMS];
};

int spa_vulkan_init_stream(struct vulkan_state *s, struct vulkan_stream *stream, enum spa_direction,
		struct spa_dict *props);

int spa_vulkan_prepare(struct vulkan_state *s);
int spa_vulkan_use_buffers(struct vulkan_state *s, struct vulkan_stream *stream, uint32_t flags,
		uint32_t n_buffers, struct spa_buffer **buffers);
int spa_vulkan_unprepare(struct vulkan_state *s);

int spa_vulkan_start(struct vulkan_state *s);
int spa_vulkan_stop(struct vulkan_state *s);
int spa_vulkan_ready(struct vulkan_state *s);
int spa_vulkan_process(struct vulkan_state *s);
int spa_vulkan_cleanup(struct vulkan_state *s);
