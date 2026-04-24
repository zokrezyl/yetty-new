load("@prelude//platforms:defs.bzl", _host_configuration = "host_configuration")

_BUILDBARN_WORKER_PROPERTIES = {
    "OSFamily": "linux",
    "container-image": "docker://ghcr.io/catthehacker/ubuntu:act-22.04@sha256:dd7654ffb01d5b7b54b23b9ce928a1f7f2d08c7b3d7e320b6574b55d7ccde78b",
}

def _remote_cache_execution_platform_impl(ctx: AnalysisContext) -> list[Provider]:
    constraints = dict()
    constraints.update(ctx.attrs.cpu_configuration[ConfigurationInfo].constraints)
    constraints.update(ctx.attrs.os_configuration[ConfigurationInfo].constraints)
    cfg = ConfigurationInfo(constraints = constraints, values = {})

    platform = ExecutionPlatformInfo(
        label = ctx.label.raw_target(),
        configuration = cfg,
        executor_config = CommandExecutorConfig(
            local_enabled = True,
            remote_enabled = True,
            remote_cache_enabled = True,
            allow_cache_uploads = True,
            max_cache_upload_mebibytes = 1024,
            use_limited_hybrid = True,
            allow_limited_hybrid_fallbacks = True,
            allow_hybrid_fallbacks_on_failure = True,
            remote_execution_properties = _BUILDBARN_WORKER_PROPERTIES,
            remote_execution_use_case = "buck2-default",
            remote_output_paths = "output_paths",
        ),
    )

    return [
        DefaultInfo(),
        ExecutionPlatformRegistrationInfo(platforms = [platform]),
    ]

remote_cache_execution_platform = rule(
    impl = _remote_cache_execution_platform_impl,
    attrs = {
        "cpu_configuration": attrs.dep(providers = [ConfigurationInfo]),
        "os_configuration": attrs.dep(providers = [ConfigurationInfo]),
    },
)

host_configuration = _host_configuration
