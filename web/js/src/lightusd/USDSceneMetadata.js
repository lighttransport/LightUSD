/**
 * Read normalized metadata values from a LightUSD scene.
 */
export function getUSDSceneMetadata(usdScene) {
  const sceneMetadata = usdScene.getSceneMetadata ? usdScene.getSceneMetadata() : {};
  const fileUpAxis = sceneMetadata.upAxis || 'Y';
  const parsedFramesPerSecond = Number(sceneMetadata.framesPerSecond);
  const parsedTimeCodesPerSecond = Number(sceneMetadata.timeCodesPerSecond);
  const parsedStartTimeCode = Number(sceneMetadata.startTimeCode);
  const parsedEndTimeCode = Number(sceneMetadata.endTimeCode);
  const framesPerSecond = Number.isFinite(parsedFramesPerSecond) && parsedFramesPerSecond > 0
    ? parsedFramesPerSecond
    : (Number.isFinite(parsedTimeCodesPerSecond) && parsedTimeCodesPerSecond > 0
      ? parsedTimeCodesPerSecond
      : 24);
  const timeCodesPerSecond = Number.isFinite(parsedTimeCodesPerSecond)
    && parsedTimeCodesPerSecond > 0
    ? parsedTimeCodesPerSecond
    : framesPerSecond;
  const startTimeCode = Number.isFinite(parsedStartTimeCode)
    ? parsedStartTimeCode
    : 0;
  const endTimeCode = Number.isFinite(parsedEndTimeCode)
    ? parsedEndTimeCode
    : 100;

  return {
    sceneMetadata,
    fileUpAxis,
    framesPerSecond,
    timeCodesPerSecond,
    startTimeCode,
    endTimeCode
  };
}
