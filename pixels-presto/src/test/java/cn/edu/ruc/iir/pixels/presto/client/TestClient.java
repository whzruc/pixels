package cn.edu.ruc.iir.pixels.presto.client;

import cn.edu.ruc.iir.pixels.common.DateUtil;
import cn.edu.ruc.iir.pixels.metadata.domain.Schema;
import com.alibaba.fastjson.JSON;

import java.util.Date;
import java.util.List;
import java.util.Scanner;

/**
 * @version V1.0
 * @Package: cn.edu.ruc.iir.pixels.presto.client
 * @ClassName: TestClient
 * @Description:
 * @author: tao
 * @date: Create in 2018-01-27 10:51
 **/
public class TestClient {

    public static void main(String[] args) {
        String action = "getTableNames";
        Scanner sc = new Scanner(System.in);
        System.out.print("Input your action: ");
        while (sc.hasNext()) {
            action = sc.next();
            System.out.println();
            TimeClient client = new TimeClient(action);
            try {
                new Thread(() -> {
                    try {
                        client.connect(18888, "127.0.0.1", "default");
                    } catch (Exception e) {
                        e.printStackTrace();
                    }
                }).start();
                System.out.println("Begin: " + DateUtil.formatTime(new Date()));
                while (true) {
                    int count = client.getQueue().size();
                    if (count > 0) {
                        String res = client.getQueue().poll();
                        if (action.equals("Time")) {
                            System.out.println(res);
                        } else if (action.equals("getSchemaNames")) {
                            List<Schema> schemas = JSON.parseArray(res, Schema.class);
                            System.out.println(schemas.size());
                        } else if (action.equals("getTableNames")) {
                            System.out.println(res);
                        } else {
                            System.out.println(res);
                        }
                        break;
                    }
                    Thread.sleep(1000);
                }
                System.out.println("End: " + DateUtil.formatTime(new Date()));
                System.out.print("Input your action: ");
            } catch (Exception e) {
                e.printStackTrace();
            }
        }
    }

}