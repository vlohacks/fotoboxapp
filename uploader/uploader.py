import requests
import os
import glob
import time

url = 'https://fotoboxserver/<uuid>'
apikey = 'theapikey'
picdir = '/mnt'

while True:
    lines = glob.glob(os.path.join(picdir, '*.jpg'))
    
    for l in lines:
        files = {'filedata' : open(l, 'rb')}
        values = {'apikey' : apikey}
        if os.path.isfile("{}.{}".format(l, 'lock')):
            print("{} is locked, trying next time".format(l))
            continue
        r = requests.post(url, files=files, data=values)
        #print(r.status_code)
        if r.status_code == 200:
            print("Successfully uploaded: {}".format(l))
            os.remove(l)
        else:
            print("Unexpected status: {}".format(r.status_code))

    time.sleep(3)
