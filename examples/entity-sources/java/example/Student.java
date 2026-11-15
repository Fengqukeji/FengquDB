package example;

// fqdb:entity prefix=student/ key=id
public class Student {
    public int id;
    public String name = "";
    private int score;
    public int getScore() { return score; }
    public void setScore(int value) { score = value; }
}
