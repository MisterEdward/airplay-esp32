Import("env")

def after_upload(source, target, env):
    print("Uploading SPIFFS image...")
    env.Execute("$PYTHONEXE -m platformio run -t uploadfs -e $PIOENV")

env.AddPostAction("upload", after_upload)
