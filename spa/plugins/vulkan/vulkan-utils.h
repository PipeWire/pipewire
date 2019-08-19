#include <vulkan/vulkan.h>

#include <spa/buffer/buffer.h>

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
	struct spa_buffer *buf;
	VkBuffer buffer;
	VkDeviceMemory memory;
};

struct vulkan_state {
	struct push_constants constants;

	VkInstance instance;

	VkPhysicalDevice physicalDevice;
	VkDevice device;

	VkPipeline pipeline;
	VkPipelineLayout pipelineLayout;
	VkShaderModule computeShaderModule;

	VkCommandPool commandPool;
	VkCommandBuffer commandBuffer;

	VkQueue queue;
	uint32_t queueFamilyIndex;
	VkFence fence;
	unsigned int prepared:1;
	unsigned int busy:1;

	VkDescriptorPool descriptorPool;
	VkDescriptorSet descriptorSet;
	VkDescriptorSetLayout descriptorSetLayout;

	struct vulkan_buffer buffers[MAX_BUFFERS];
	uint32_t n_buffers;

	uint32_t bufferSize;
	uint32_t buffer_id;
};

int spa_vulkan_prepare(struct vulkan_state *s);
int spa_vulkan_use_buffers(struct vulkan_state *s, uint32_t flags,
		uint32_t n_buffers, struct spa_buffer **buffers);
int spa_vulkan_unprepare(struct vulkan_state *s);

int spa_vulkan_start(struct vulkan_state *s);
int spa_vulkan_stop(struct vulkan_state *s);
int spa_vulkan_ready(struct vulkan_state *s);
int spa_vulkan_process(struct vulkan_state *s, uint32_t buffer_id);
int spa_vulkan_cleanup(struct vulkan_state *s);
