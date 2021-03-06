static GstCheckABIStruct list[] = {
  {"GstAllocationParams", sizeof (GstAllocationParams), 64},
  {"GstAllocator", sizeof (GstAllocator), 176},
  {"GstAllocatorClass", sizeof (GstAllocatorClass), 232},
  {"GstBinClass", sizeof (GstBinClass), 576},
  {"GstBin", sizeof (GstBin), 376},
  {"GstBuffer", sizeof (GstBuffer), 112},
  {"GstBufferPoolAcquireParams", sizeof (GstBufferPoolAcquireParams), 64},
  {"GstBufferPool", sizeof (GstBufferPool), 136},
  {"GstBufferPoolClass", sizeof (GstBufferPoolClass), 288},
  {"GstBusClass", sizeof (GstBusClass), 232},
  {"GstBus", sizeof (GstBus), 128},
  {"GstCaps", sizeof (GstCaps), 64},
  {"GstClockClass", sizeof (GstClockClass), 264},
  {"GstClockEntry", sizeof (GstClockEntry), 112},
  {"GstClock", sizeof (GstClock), 128},
  {"GstControlBinding", sizeof (GstControlBinding), 152},
  {"GstControlBindingClass", sizeof (GstControlBindingClass), 248},
  {"GstControlSource", sizeof (GstControlSource), 136},
  {"GstControlSourceClass", sizeof (GstControlSourceClass), 216},
  {"GstDebugCategory", sizeof (GstDebugCategory), 24},
  {"GstElementClass", sizeof (GstElementClass), 488},
  {"GstElement", sizeof (GstElement), 264},
  {"GstEvent", sizeof (GstEvent), 88},
  {"GstFormatDefinition", sizeof (GstFormatDefinition), 32},
  {"GstGhostPadClass", sizeof (GstGhostPadClass), 272},
  {"GstGhostPad", sizeof (GstGhostPad), 536},
  {"GstIterator", sizeof (GstIterator), 120},
  {"GstMemory", sizeof (GstMemory), 112},
  {"GstMapInfo", sizeof (GstMapInfo), 104},
  {"GstMessage", sizeof (GstMessage), 120},
  {"GstMeta", sizeof (GstMeta), 16},
  {"GstMetaTransformCopy", sizeof (GstMetaTransformCopy), 24},
  {"GstMiniObject", sizeof (GstMiniObject), 64},
  {"GstObjectClass", sizeof (GstObjectClass), 184},
  {"GstObject", sizeof (GstObject), 88},
  {"GstPadClass", sizeof (GstPadClass), 232},
  {"GstPad", sizeof (GstPad), 520},
  {"GstPadProbeInfo", sizeof (GstPadProbeInfo), 72},
  {"GstPadTemplateClass", sizeof (GstPadTemplateClass), 224},
  {"GstPadTemplate", sizeof (GstPadTemplate), 144},
  {"GstParamSpecFraction", sizeof (GstParamSpecFraction), 96},
  {"GstPipelineClass", sizeof (GstPipelineClass), 608},
  {"GstPipeline", sizeof (GstPipeline), 440},
  {"GstPluginDesc", sizeof (GstPluginDesc), 112},
  {"GstProxyPadClass", sizeof (GstProxyPadClass), 240},
  {"GstProxyPad", sizeof (GstProxyPad), 528},
  {"GstQuery", sizeof (GstQuery), 72},
  {"GstRegistryClass", sizeof (GstRegistryClass), 184},
  {"GstRegistry", sizeof (GstRegistry), 96},
  {"GstSegment", sizeof (GstSegment), 120},
  {"GstStaticCaps", sizeof (GstStaticCaps), 48},
  {"GstStaticPadTemplate", sizeof (GstStaticPadTemplate), 64},
  {"GstStructure", sizeof (GstStructure), 16},
  {"GstSystemClockClass", sizeof (GstSystemClockClass), 296},
  {"GstSystemClock", sizeof (GstSystemClock), 168},
  {"GstTagList", sizeof (GstTagList), 64,},
  {"GstTaskClass", sizeof (GstTaskClass), 232},
  {"GstTask", sizeof (GstTask), 200},
  {"GstTaskPoolClass", sizeof (GstTaskPoolClass), 248},
  {"GstTaskPool", sizeof (GstTaskPool), 128},
  {"GstTimedValue", sizeof (GstTimedValue), 16},
  {"GstTypeFind", sizeof (GstTypeFind), 64},
  {"GstValueTable", sizeof (GstValueTable), 64},
  {NULL, 0, 0}
};
